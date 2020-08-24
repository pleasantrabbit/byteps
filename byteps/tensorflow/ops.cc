// Copyright 2016 The TensorFlow Authors. All Rights Reserved.
// Modifications copyright (C) 2018 Uber Technologies, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// =============================================================================

#include <memory>
#include <queue>
#include <chrono>
#include <thread>
#include <unordered_map>
#include <sstream>
#include <fstream>

#include <cuda_runtime.h>
#include <cuda.h>

#include "ops.h"
#include "../common/logging.h"

#include "tensorflow/core/framework/op.h"
#include "tensorflow/core/framework/shape_inference.h"
#include "tensorflow/core/framework/op_kernel.h"

#include "tensorflow/compiler/tf2xla/shape_util.h"
#include "tensorflow/compiler/tf2xla/xla_helpers.h"
#include "tensorflow/compiler/tf2xla/xla_op_kernel.h"
#include "tensorflow/compiler/tf2xla/xla_op_registry.h"
#include "tensorflow/compiler/xla/client/xla_builder.h"
#include "tensorflow/compiler/xla/service/custom_call_target_registry.h"
#include "tensorflow/compiler/tf2xla/type_util.h"
#include "tensorflow/compiler/xla/shape_util.h"
#include "tensorflow/compiler/xla/types.h"

#include "tensorflow/compiler/tf2xla/kernels/tensor_list_utils.h"

#include "tensorflow/core/common_runtime/gpu/gpu_id.h"
#include "tensorflow/core/common_runtime/gpu/gpu_init.h"
#include "tensorflow/core/common_runtime/gpu/gpu_id_utils.h"
#include "tensorflow/core/common_runtime/gpu/gpu_bfc_allocator.h"
#include "tensorflow/core/common_runtime/gpu/gpu_cudamalloc_allocator.h"

// #include <cassert>

using namespace byteps;

namespace byteps {
namespace tensorflow {

namespace {

int printMatOnGPU(std::string &name, const void *buffer, int num_elem) {
  std::this_thread::sleep_for(std::chrono::seconds(2));
  std::ofstream outfile;
  int my_rank = common::byteps_rank();
  outfile.open("output-" + std::to_string(my_rank), std::ios_base::out | std::ios_base::app);
  outfile << "tensor_name: " << name << std::endl;

  float *host_buf = nullptr;
  host_buf = (float *)malloc(num_elem * sizeof(float));
  cudaMemcpy((void *) host_buf, buffer, sizeof(float) * num_elem, cudaMemcpyDeviceToHost);
  for (int i = 0; i < num_elem; i++) {
      outfile << *(host_buf + i) << ", ";
  }
  outfile << std::endl;
  free(host_buf);

  return 0;
}

::tensorflow::Status ConvertStatus(const common::Status& status) {
  switch (status.type()) {
    case common::OK:
      return ::tensorflow::Status::OK();
    case common::UNKNOWN_ERROR:
      return ::tensorflow::errors::Unknown(status.reason());
    case common::PRECONDITION_ERROR:
      return ::tensorflow::errors::FailedPrecondition(status.reason());
    case common::ABORTED:
      return ::tensorflow::errors::Aborted(status.reason());
    case common::INVALID_ARGUMENT:
      return ::tensorflow::errors::InvalidArgument(status.reason());
    default:
      return ::tensorflow::errors::Unknown("Unknown error.");
  }
}

int GetDeviceID(::tensorflow::OpKernelContext* context) {
  int device = CPU_DEVICE_ID;
  if (context->device() != nullptr &&
      context->device()->tensorflow_gpu_device_info() != nullptr) {
    device = context->device()->tensorflow_gpu_device_info()->gpu_id;
  }
  return device;
}

// Define all types for TensorUtil.
const common::DataType ConvertDType(int dtype) {
  switch (dtype) {
    case ::tensorflow::DT_UINT8:
      return common::BYTEPS_UINT8;
    case ::tensorflow::DT_INT8:
      return common::BYTEPS_INT8;
    // case ::tensorflow::DT_UINT16:
    //   return common::BYTEPS_UINT16;
    // case ::tensorflow::DT_INT16:
    //   return common::BYTEPS_INT16;
    case ::tensorflow::DT_INT32:
      return common::BYTEPS_INT32;
    case ::tensorflow::DT_INT64:
      return common::BYTEPS_INT64;
    case ::tensorflow::DT_HALF:
      return common::BYTEPS_FLOAT16;
    case ::tensorflow::DT_FLOAT:
      return common::BYTEPS_FLOAT32;
    case ::tensorflow::DT_DOUBLE:
      return common::BYTEPS_FLOAT64;
    // case ::tensorflow::DT_BOOL:
    //   return common::BYTEPS_BOOL;
    default:
      throw std::logic_error("Invalid tensor type.");
  }
}

}  // namespace

TFReadyEvent::TFReadyEvent(::tensorflow::DeviceContext* device_context) {
  auto executor = device_context->stream()->parent();
  auto ready_event = new perftools::gputools::Event(executor);
  ready_event->Init();
  device_context->stream()->ThenRecordEvent(ready_event);
  event_ = std::shared_ptr<perftools::gputools::Event>(ready_event);
}

bool TFReadyEvent::Ready() const {
  return event_->PollForStatus() !=
         perftools::gputools::Event::Status::kPending;
}

XlaReadyEvent::XlaReadyEvent(cudaStream_t stream) {
  cudaEventCreateWithFlags(
          &cuda_event_, cudaEventBlockingSync | cudaEventDisableTiming);
  cudaEventRecord(cuda_event_, stream);
}

bool XlaReadyEvent::Ready() const {
  auto status = cudaEventQuery(cuda_event_);
  if (status == cudaErrorNotReady) {
    return false;
  }
  return true;
}

TFTensor::TFTensor(::tensorflow::Tensor& tensor) : tensor_(tensor) {}

const common::DataType TFTensor::dtype() const {
  return ConvertDType(tensor_.dtype());
}

const common::TensorShape TFTensor::shape() const {
  common::TensorShape shape;
  for (auto dim : tensor_.shape()) {
    shape.AddDim(dim.size);
  }
  return shape;
}

const void* TFTensor::data() const {
  return (const void*)tensor_.tensor_data().data();
}

int64_t TFTensor::size() const { return (int64_t)tensor_.tensor_data().size(); }

XlaTensor::XlaTensor(void *data, int64_t num_elem,
                         ::tensorflow::DataType tf_dtype, int64_t size) {
    _data = data;
    _num_elem = num_elem;
    _tf_dtype = tf_dtype;
    _size = size;
}

const common::DataType XlaTensor::dtype() const {
  return ConvertDType(_tf_dtype);
}

const common::TensorShape XlaTensor::shape() const {
  common::TensorShape shape;
  shape.AddDim(_num_elem);
  return shape;
}

const void* XlaTensor::data() const {
  return (const void*)_data;
}

int64_t XlaTensor::size() const { return _size; }


// On GPU this event will signal that data is ready, and tensors are
// allocated.
common::ReadyEvent* RecordReadyEvent(::tensorflow::OpKernelContext* context) {
  auto device_context = context->op_device_context();
  if (device_context != nullptr) {
    return new TFReadyEvent(device_context);
  }
  return nullptr;
}

std::shared_ptr<common::ReadyEvent> RecordReadyEvent(cudaStream_t stream) {
  return std::make_shared<XlaReadyEvent>(stream);

}

extern "C" void byteps_tensorflow_declare_tensor(char* name) {
  std::string tensor_name(name);
  common::IsTensorDeclared(tensor_name);
  return;
}

void StartTask(::tensorflow::OpKernelContext* context,
               ::tensorflow::AsyncOpKernel::DoneCallback done,
               std::string node_name, std::shared_ptr<TFTensor> byteps_input,
               std::shared_ptr<TFTensor> byteps_output,
               std::shared_ptr<common::ReadyEvent> ready_event) {
  auto& byteps_context = common::GetContextFromName(node_name);
  auto device = GetDeviceID(context);
  auto size = byteps_input->size();
  auto dtype = byteps_input->dtype();
  void* cpubuff = (device == CPU_DEVICE_ID)
                      ? const_cast<void*>(byteps_input->data())
                      : nullptr;
  common::InitTensor(byteps_context, size, dtype, cpubuff);
  // ASSERTF(0 == 1, "pos 1");

  auto queue_list = common::GetPushQueueList(device);
  auto queue_list_pull = common::GetPullQueueList(device);
  queue_list->insert(queue_list->end(), queue_list_pull->begin(),
                     queue_list_pull->end());

  // TODO: assign priority based on topological sort
  auto enqueue_result =
      EnqueueTensor(byteps_context, byteps_input, byteps_output, ready_event,
                    device, -byteps_context.declared_key, 0,
                    [context, done](const common::Status& status) {
                      context->SetStatus(ConvertStatus(status));
                      done();
                    },
                    queue_list);
  OP_REQUIRES_OK_ASYNC(context, ConvertStatus(enqueue_result), done);
}

class BytePSPushPullOp : public ::tensorflow::AsyncOpKernel {
  private:
     std::string input_tensor_name;
 public:
  explicit BytePSPushPullOp(::tensorflow::OpKernelConstruction* context)
      : AsyncOpKernel(context) {
          context->GetAttr("input_name", &input_tensor_name);
      }

  void ComputeAsync(::tensorflow::OpKernelContext* context,
                    DoneCallback done) override {
    OP_REQUIRES_OK_ASYNC(context, ConvertStatus(common::CheckInitialized()),
                         done);

    auto tensor = context->input(0);
    ::tensorflow::Tensor* output;
    OP_REQUIRES_OK_ASYNC(
        context, context->allocate_output(0, tensor.shape(), &output), done);
    // ReadyEvent makes sure input tensor is ready, and output is allocated.
    auto ready_event =
        std::shared_ptr<common::ReadyEvent>(RecordReadyEvent(context));
    auto bps_input = std::make_shared<TFTensor>(tensor);
    auto bps_output = std::make_shared<TFTensor>(*output);
    auto node_name = name();
    std::string tmp_name;
    if (input_tensor_name == "default_tensor_name") {
        tmp_name = node_name;
    } else {
        tmp_name = input_tensor_name;
    }
    auto& bps_context = common::GetContextFromName(tmp_name);
    if (bps_context.initialized) {
      StartTask(context, done, tmp_name, bps_input, bps_output, ready_event);
    } else {
      std::thread t(StartTask, context, done, tmp_name, bps_input, bps_output,
                    ready_event);
      t.detach();
    }
  }
};

REGISTER_KERNEL_BUILDER(Name("BytepsPushPull").Device(::tensorflow::DEVICE_CPU),
                        BytePSPushPullOp);
REGISTER_KERNEL_BUILDER(Name("BytepsPushPull").Device(::tensorflow::DEVICE_GPU),
                        BytePSPushPullOp);

REGISTER_OP("BytepsPushPull")
    .Attr("T: {int32, int64, float16, float32, float64}")
    .Attr("input_name: string = 'default_tensor_name'")
    .Input("tensor: T")
    .Output("sum: T")
    .SetShapeFn([](::tensorflow::shape_inference::InferenceContext* c) {
      c->set_output(0, c->input(0));
      return ::tensorflow::Status::OK();
    })
    .Doc(R"doc(
Perform an PushPull on a tensor. All other processes that do a reduction
on a tensor with the same name must have the same dimension for that tensor.
Tensors are reduced with other tensors that have the same node name for the
push_pull.
Arguments
    tensor:     A tensor to reduce.
Output
    sum:    A tensor with the same shape as `tensor`, summed across all processes.
)doc");


class BytepsPushPullXlaOp : public ::tensorflow::XlaOpKernel {
  public:
    explicit BytepsPushPullXlaOp(::tensorflow::OpKernelConstruction* context) : ::tensorflow::XlaOpKernel(context) {
      context->GetAttr("input_name", &input_tensor_name);
      // OP_REQUIRES_OK(context, context->GetAttr("type", &dst_dtype_));
      // OP_REQUIRES_OK(context, DataTypeToPrimitiveType(dst_dtype_, &dst_type_));
    }
    ~BytepsPushPullXlaOp() override = default;

    void Compile(::tensorflow::XlaOpKernelContext* context) override {
      OP_REQUIRES_OK(context, ConvertStatus(common::CheckInitialized()));

      xla::XlaOp input_tensor = context->Input(0);
      // auto shape_or = context->InputXlaShape(0);
      auto input_tensor_xla_shape_or = context->InputXlaShape(0);
      xla::Shape output_tensor_shape = input_tensor_xla_shape_or.ValueOrDie();

      // auto input_shape = context->InputShape(0);
      // xla::Shape input_xla_shape = TensorShapeToXLAShape(dst_type_, input_shape);
      std::cout << " x2682  " << output_tensor_shape.ToString(true) << std::endl;
      std::cout << " x2682  has_layout? " << xla::LayoutUtil::HasLayout(output_tensor_shape) << std::endl;
      std::cout << " x2682  num dimensions " << output_tensor_shape.rank() << std::endl;
      std::cout << " x2682  dimensions are ";
      for (int i = 0; i < output_tensor_shape.rank(); i++) {
        std::cout << " " << output_tensor_shape.dimensions(i) ;
      }
      std::cout << std::endl;
      std::cout << " x2682  memory_space " << output_tensor_shape.layout().memory_space() << std::endl;
      // std::cout << " x2682  " << input_xla_shape.ToProto() << std::endl;
      std::cout << " x2682  end shape test" << std::endl;

      std::cout << " x2682  pos 1 " << std::endl;
      // OP_REQUIRES_OK(context, shape_or.status());

      // OP_REQUIRES_OK(
      //   context, context->allocate_output(0, tensor.shape(), &output));
      auto node_name = name();
      std::string tmp_name;
      if (input_tensor_name == "default_tensor_name") {
        tmp_name = node_name;
      } else {
        tmp_name = input_tensor_name;
      }
      // auto ready_event =
      //   std::shared_ptr<common::ReadyEvent>(RecordReadyEvent(context->op_kernel_context()));
      // std::cout << "x2682 device_id " << GetDeviceID(context->op_kernel_context()) << std::endl;

      std::stringstream ss;
      // ss << tmp_name << " " << context->op_kernel_context();
      // ss << tmp_name << " " << ready_event;
      ss << tmp_name;
      // ss << " " << ::tensorflow::EncodePrimitiveTypeAsDataType(output_tensor_shape.element_type()).ValueOrDie();
      ss << " " << context->input_type(0);
      ss << " " << xla::ShapeUtil::ByteSizeOfPrimitiveType(output_tensor_shape.element_type());
      ss << " " << output_tensor_shape.rank();
      for (int i = 0; i < output_tensor_shape.rank(); i++) {
        ss << " " << output_tensor_shape.dimensions(i) ;
      }
      ss << std::endl;
      std::cout << " x2682  pos 2 " << std::endl;
      std::cout << " x2682  passing opaque: " << ss.str() << std::endl;
      context->SetOutput(
        0, xla::CustomCall(context->builder(),
          /*call_target_name=*/"StartTaskWrapper",
          {input_tensor}, input_tensor_xla_shape_or.ValueOrDie(), ss.str()));
      // private:
      //   TF_DISALLOW_COPY_AND_ASSIGN(BytepsPushPullXlaOp);
      std::cout << " x2682  pos 3 " << std::endl;
    }
  private:
     std::string input_tensor_name;
  // protected:
  //    DataType dst_dtype_;
  //    xla::PrimitiveType  dst_type_;
};

REGISTER_XLA_OP(Name("BytepsPushPull"), BytepsPushPullXlaOp);

class BytepsPushPullBlockingXlaOp : public ::tensorflow::XlaOpKernel {
  public:
    explicit BytepsPushPullBlockingXlaOp(::tensorflow::OpKernelConstruction* context) : ::tensorflow::XlaOpKernel(context) {
      context->GetAttr("input_name", &input_tensor_name);
      // OP_REQUIRES_OK(context, context->GetAttr("type", &dst_dtype_));
      // OP_REQUIRES_OK(context, DataTypeToPrimitiveType(dst_dtype_, &dst_type_));
    }
    ~BytepsPushPullBlockingXlaOp() override = default;

    void Compile(::tensorflow::XlaOpKernelContext* context) override {
      OP_REQUIRES_OK(context, ConvertStatus(common::CheckInitialized()));

      xla::XlaOp input_tensor = context->Input(0);
      // auto shape_or = context->InputXlaShape(0);
      auto input_tensor_xla_shape_or = context->InputXlaShape(0);
      xla::Shape output_tensor_shape = input_tensor_xla_shape_or.ValueOrDie();

      // auto input_shape = context->InputShape(0);
      // xla::Shape input_xla_shape = TensorShapeToXLAShape(dst_type_, input_shape);
      std::cout << " x2682  " << output_tensor_shape.ToString(true) << std::endl;
      std::cout << " x2682  has_layout? " << xla::LayoutUtil::HasLayout(output_tensor_shape) << std::endl;
      std::cout << " x2682  num dimensions " << output_tensor_shape.rank() << std::endl;
      std::cout << " x2682  dimensions are ";
      for (int i = 0; i < output_tensor_shape.rank(); i++) {
        std::cout << " " << output_tensor_shape.dimensions(i) ;
      }
      std::cout << std::endl;
      std::cout << " x2682  memory_space " << output_tensor_shape.layout().memory_space() << std::endl;
      // std::cout << " x2682  " << input_xla_shape.ToProto() << std::endl;
      std::cout << " x2682  end shape test" << std::endl;

      std::cout << " x2682  pos 1 " << std::endl;
      // OP_REQUIRES_OK(context, shape_or.status());

      // OP_REQUIRES_OK(
      //   context, context->allocate_output(0, tensor.shape(), &output));
      auto node_name = name();
      std::string tmp_name;
      if (input_tensor_name == "default_tensor_name") {
        tmp_name = node_name;
      } else {
        tmp_name = input_tensor_name;
      }
      // auto ready_event =
      //   std::shared_ptr<common::ReadyEvent>(RecordReadyEvent(context->op_kernel_context()));
      // std::cout << "x2682 device_id " << GetDeviceID(context->op_kernel_context()) << std::endl;

      std::stringstream ss;
      // ss << tmp_name << " " << context->op_kernel_context();
      // ss << tmp_name << " " << ready_event;
      ss << tmp_name;
      // ss << " " << ::tensorflow::EncodePrimitiveTypeAsDataType(output_tensor_shape.element_type()).ValueOrDie();
      ss << " " << context->input_type(0);
      ss << " " << xla::ShapeUtil::ByteSizeOfPrimitiveType(output_tensor_shape.element_type());
      ss << " " << output_tensor_shape.rank();
      for (int i = 0; i < output_tensor_shape.rank(); i++) {
        ss << " " << output_tensor_shape.dimensions(i) ;
      }
      ss << std::endl;
      std::cout << " x2682  pos 2 " << std::endl;
      std::cout << " x2682  passing opaque: " << ss.str() << std::endl;
      context->SetOutput(
        0, xla::CustomCall(context->builder(),
          /*call_target_name=*/"StartTaskBlockingWrapper",
          {input_tensor}, input_tensor_xla_shape_or.ValueOrDie(), ss.str()));
      // private:
      //   TF_DISALLOW_COPY_AND_ASSIGN(BytepsPushPullXlaOp);
      std::cout << " x2682  pos 3 " << std::endl;
    }
  private:
     std::string input_tensor_name;
  // protected:
  //    DataType dst_dtype_;
  //    xla::PrimitiveType  dst_type_;
};

void StartTaskBlockingXla(::tensorflow::OpKernelContext* context,
               std::string node_name, std::shared_ptr<common::Tensor> byteps_input,
               std::shared_ptr<common::Tensor> byteps_output,
               std::shared_ptr<common::ReadyEvent> ready_event) {
  std::cout << " x2682  pos 11 inside StartTaskBlockingXla" << std::endl;
  auto& byteps_context = common::GetContextFromName(node_name);
  std::cout << " x2682  pos 12 " << std::endl;
  // auto device = GetDeviceID(context);
  // auto device = 0;
  int device;
  CUDA_CALL(cudaGetDevice(&device));
  int myrank =  common::byteps_rank();
  std::cout << " x2682 rank " << common::byteps_rank() << " device: " << device << std::endl;
  std::cout << " x2682  pos 13 " << std::endl;
  auto size = byteps_input->size();
  std::cout << " x2682  pos 14 " << std::endl;
  auto dtype = byteps_input->dtype();
  std::cout << " x2682  pos 15 " << std::endl;
  // void* cpubuff = (device == CPU_DEVICE_ID)
  //                     ? const_cast<void*>(byteps_input->data())
  //                     : nullptr;
  void* cpubuff = nullptr;
  // void* cpubuff = const_cast<void*>(byteps_input->data());
  common::InitTensor(byteps_context, size, dtype, cpubuff);

  auto queue_list = common::GetPushQueueList(device);
  auto queue_list_pull = common::GetPullQueueList(device);
  queue_list->insert(queue_list->end(), queue_list_pull->begin(),
                     queue_list_pull->end());

  // std::mutex mtx;
  // std::condition_variable cv;
  std::cout << " x2682  pos 16 before EnqueueTensor name: " << node_name << " rank: " << myrank << std::endl;
  // TODO: assign priority based on topological sort

  std::string name_key(node_name);
  std::replace(name_key.begin(), name_key.end(), '/', '_');
  std::cout << " x2682  pos 16 before EnqueueTensor name_key: " << name_key << " rank: " << myrank << std::endl;
  _name_to_done_args[name_key].is_done = false;
  auto enqueue_result =
      EnqueueTensor(byteps_context, byteps_input, byteps_output, ready_event,
                    device, -byteps_context.declared_key, 0,
                    [name_key](const common::Status& status) {
                      // context->SetStatus(ConvertStatus(status));
                      auto& args = _name_to_done_args[name_key];
                      {
                        std::unique_lock<std::mutex> lk(args.mtx);
                        args.is_done = true;
                      }
                      args.cv.notify_one();
                      std::cout << "inside callback name_key: " << name_key <<" rank: " << common::byteps_rank() << " notified" << std::endl;
                    },
                    queue_list);
  {
    auto& args = _name_to_done_args[name_key];
    std::unique_lock<std::mutex> lk(args.mtx);
    args.cv.wait(lk, [&args]{return args.is_done;});
  }
  std::cout << " x2682  blocking pos 17 after EnqueueTensor name: " << node_name << " rank: " << myrank << std::endl;
}

void StartTaskBlockingWrapper(CUstream stream, void** buffers,
                      const char* opaque, size_t opaque_len) {
    std::cout << " x2682  pos 4 " << std::endl;
    void *a = buffers[0];
    std::cout << " x2682  pos 5 " << std::endl;
    void *b = buffers[1];
    std::cout << " x2682  pos 6 " << std::endl;
    std::cout << " x2682  pos 7 " << std::endl;
    std::cout << " x2682  pos 8 " << std::endl;

    std::cout << " x2682  received opaque: " << opaque << std::endl;
    std::stringstream ss(opaque);
    std::string tmp_name;
    ::tensorflow::OpKernelContext* context = nullptr;

    ss >> tmp_name;
    ::tensorflow::DataType dt_type;
    int tmp_dt_type;
    ss >> std::dec >> tmp_dt_type;
    dt_type = static_cast<::tensorflow::DataType>(tmp_dt_type);
    size_t elem_size;
    ss >> elem_size;
    int ndim = 0;
    ss >> std::dec >> ndim;
    size_t buffer_size = 0;
    size_t num_elem = 1;
    for (int i = 0; i < ndim; i++) {
      size_t dim;
      ss >> std::dec >> dim;
      num_elem *= dim;
      std::cout << " dim " << dim;
    }

    buffer_size = elem_size * num_elem;
    std::cout << " ndim " << ndim << " num_elem " << num_elem << " buffer_size " << buffer_size << std::endl;
    ::tensorflow::PlatformGpuId platform_gpu_id(0);

    auto bps_input = std::make_shared<XlaTensor>(buffers[0], num_elem, dt_type, buffer_size);
    auto ready_event =
        std::shared_ptr<common::ReadyEvent>(RecordReadyEvent(stream));

    ::tensorflow::Tensor outputTensor(dt_type, ::tensorflow::TensorShape({num_elem}));
    auto bps_output = std::make_shared<XlaTensor>(buffers[1], num_elem, dt_type, buffer_size);

    StartTaskBlockingXla(context, tmp_name, bps_input, bps_output, ready_event);

    // cudaMemcpyAsync(buffers[1], buffers[0], buffer_size, cudaMemcpyDeviceToDevice, stream);
    // printMatOnGPU(tmp_name, buffers[1], num_elem);
    std::cout << " x2682  pushpullblocking " << std::endl;
}

XLA_REGISTER_CUSTOM_CALL_TARGET(StartTaskBlockingWrapper, "CUDA");

REGISTER_XLA_OP(Name("BytepsPushPullBlocking"), BytepsPushPullBlockingXlaOp);
REGISTER_KERNEL_BUILDER(Name("BytepsPushPullBlocking").Device(::tensorflow::DEVICE_CPU),
                        BytePSPushPullOp);
REGISTER_KERNEL_BUILDER(Name("BytepsPushPullBlocking").Device(::tensorflow::DEVICE_GPU),
                        BytePSPushPullOp);

REGISTER_OP("BytepsPushPullBlocking")
    .Attr("T: {int32, int64, float16, float32, float64}")
    .Attr("input_name: string = 'default_tensor_name'")
    .Input("tensor: T")
    .Output("sum: T")
    .SetShapeFn([](::tensorflow::shape_inference::InferenceContext* c) {
      c->set_output(0, c->input(0));
      return ::tensorflow::Status::OK();
    });

void StartTaskXla(::tensorflow::OpKernelContext* context,
               std::string node_name, std::shared_ptr<common::Tensor> byteps_input,
               std::shared_ptr<common::Tensor> byteps_output,
               std::shared_ptr<common::ReadyEvent> ready_event) {
  std::cout << " x2682  pos 11 inside StartTaskXla" << std::endl;
  auto& byteps_context = common::GetContextFromName(node_name);
  std::cout << " x2682  pos 12 " << std::endl;
  // auto device = GetDeviceID(context);
  // auto device = 0;
  int device;
  CUDA_CALL(cudaGetDevice(&device));
  int myrank =  common::byteps_rank();
  std::cout << " x2682 rank " << common::byteps_rank() << " device: " << device << std::endl;
  std::cout << " x2682  pos 13 " << std::endl;
  auto size = byteps_input->size();
  std::cout << " x2682  pos 14 " << std::endl;
  auto dtype = byteps_input->dtype();
  std::cout << " x2682  pos 15 " << std::endl;
  // void* cpubuff = (device == CPU_DEVICE_ID)
  //                     ? const_cast<void*>(byteps_input->data())
  //                     : nullptr;
  void* cpubuff = nullptr;
  // void* cpubuff = const_cast<void*>(byteps_input->data());
  common::InitTensor(byteps_context, size, dtype, cpubuff);

  auto queue_list = common::GetPushQueueList(device);
  auto queue_list_pull = common::GetPullQueueList(device);
  queue_list->insert(queue_list->end(), queue_list_pull->begin(),
                     queue_list_pull->end());

  std::mutex mtx;
  std::condition_variable cv;
  std::cout << " x2682  pos 16 before EnqueueTensor name: " << node_name << " rank: " << myrank << std::endl;
  // TODO: assign priority based on topological sort

  std::string name_key(node_name);
  std::replace(name_key.begin(), name_key.end(), '/', '_');
  std::cout << " x2682  pos 16 before EnqueueTensor name_key: " << name_key << " rank: " << myrank << std::endl;
  _name_to_done_args[name_key].is_done = false;
  _name_to_done_args[name_key].bps_out_buf = byteps_output->data();
  bool& is_done = _name_to_done_args[name_key].is_done;
  auto enqueue_result =
      EnqueueTensor(byteps_context, byteps_input, byteps_output, ready_event,
                    device, -byteps_context.declared_key, 0,
                    [name_key](const common::Status& status) {
                      // context->SetStatus(ConvertStatus(status));
                      auto& args = _name_to_done_args[name_key];
                      {
                        std::unique_lock<std::mutex> lk(args.mtx);
                        args.is_done = true;
                      }
                      args.cv.notify_one();
                      std::cout << "inside callback name_key: " << name_key <<" rank: " << common::byteps_rank() << " notified" << std::endl;
                    },
                    queue_list);
  // {
  //     std::unique_lock<std::mutex> lk(mtx);
  //     cv.wait(lk, [&is_done]{return is_done;});
  // }
  std::cout << " x2682  pos 17 after EnqueueTensor name: " << node_name << " rank: " << myrank << std::endl;
}

// void StartTaskWrapper(void* out, const void** in) {
void StartTaskWrapper(CUstream stream, void** buffers,
                      const char* opaque, size_t opaque_len) {
    cudaStreamSynchronize(stream);

    std::cout << " x2682  pos 4 " << std::endl;
    void *a = buffers[0];
    std::cout << " x2682  pos 5 " << std::endl;
    void *b = buffers[1];
    std::cout << " x2682  pos 6 " << std::endl;
    std::cout << " x2682  pos 7 " << std::endl;
    std::cout << " x2682  pos 8 " << std::endl;

    std::cout << " x2682  received opaque: " << opaque << std::endl;
    std::stringstream ss(opaque);
    std::string tmp_name;
    ::tensorflow::OpKernelContext* context = nullptr;

    ss >> tmp_name;
    ::tensorflow::DataType dt_type;
    int tmp_dt_type;
    ss >> std::dec >> tmp_dt_type;
    dt_type = static_cast<::tensorflow::DataType>(tmp_dt_type);
    size_t elem_size;
    ss >> elem_size;
    int ndim = 0;
    ss >> std::dec >> ndim;
    size_t buffer_size = 0;
    size_t num_elem = 1;
    for (int i = 0; i < ndim; i++) {
      size_t dim;
      ss >> std::dec >> dim;
      num_elem *= dim;
      std::cout << " dim " << dim;
    }

    printMatOnGPU(tmp_name, buffers[0], num_elem);

    buffer_size = elem_size * num_elem;
    std::cout << " ndim " << ndim << " num_elem " << num_elem << " buffer_size " << buffer_size << std::endl;
    ::tensorflow::PlatformGpuId platform_gpu_id(0);

    ::tensorflow::GPUMemAllocator *sub_allocator =
      new ::tensorflow::GPUMemAllocator(
        ::tensorflow::GpuIdUtil::ExecutorForPlatformGpuId(platform_gpu_id).ValueOrDie(),
        platform_gpu_id, false /*use_unified_memory*/, {}, {});
#if 0
    //////// start
    ::tensorflow::GPUMemAllocator *sub_allocator =
      new ::tensorflow::GPUMemAllocator(
        ::tensorflow::GpuIdUtil::ExecutorForPlatformGpuId(platform_gpu_id).ValueOrDie(),
        platform_gpu_id, false /*use_unified_memory*/, {}, {});

    ::tensorflow::GPUBFCAllocator *input_allocator =
      new ::tensorflow::GPUBFCAllocator(sub_allocator, buffer_size, "GPU_0_bfc");

    ::tensorflow::Tensor inputTensor(input_allocator, dt_type, ::tensorflow::TensorShape({num_elem}));

    void *inputTensor_flat = const_cast<void *>((const void *)(inputTensor.tensor_data().data()));
    //////// end
#endif

    //////// start
    ::tensorflow::Tensor inputTensor(dt_type, ::tensorflow::TensorShape({num_elem}));
    // auto bps_input = std::make_shared<TFTensor>(inputTensor);
    auto bps_input = std::make_shared<XlaTensor>(buffers[0], num_elem, dt_type, buffer_size);
    // void *inputTensor_flat = const_cast<void *>(bps_input->data());
    // cudaError_t e = cudaHostRegister(inputTensor_flat, buffer_size, cudaHostRegisterMapped);
    // void *gpu_ptr = nullptr;
    // CUDA_CALL(cudaHostGetDevicePointer(&gpu_ptr, inputTensor_flat, 0));
    //////// end

    // cudaMemcpyAsync(gpu_ptr, buffers[0], buffer_size, cudaMemcpyDeviceToDevice, stream);
    std::cout << " x2682  pos 9 " << std::endl;
    auto ready_event =
        std::shared_ptr<common::ReadyEvent>(RecordReadyEvent(stream));

    ::tensorflow::Tensor outputTensor(dt_type, ::tensorflow::TensorShape({num_elem}));
    // auto bps_output = std::make_shared<TFTensor>(outputTensor);
    auto bps_output = std::make_shared<XlaTensor>(buffers[1], num_elem, dt_type, buffer_size);

    //////// start
    // void *outputTensor_flat = const_cast<void *>(bps_output->data());
    // cudaError_t ee = cudaHostRegister(outputTensor_flat, buffer_size, cudaHostRegisterMapped);
    // void *out_gpu_ptr = nullptr;
    // CUDA_CALL(cudaHostGetDevicePointer(&out_gpu_ptr, inputTensor_flat, 0));
    //////// end

    std::cout << " x2682  pos 10 " << std::endl;
    // auto& bps_context = common::GetContextFromName(tmp_name);

    StartTaskXla(context, tmp_name, bps_input, bps_output, ready_event);

    // cudaMemcpyAsync(buffers[1], const_cast<void *>(bps_output->data()), buffer_size, cudaMemcpyDeviceToDevice, stream);
    // cudaMemcpyAsync(buffers[1], out_gpu_ptr, buffer_size, cudaMemcpyDeviceToDevice, stream);
    std::cout << " x2682  pos end " << std::endl;
}

XLA_REGISTER_CUSTOM_CALL_TARGET(StartTaskWrapper, "CUDA");

void SyncTensorCustomOp(CUstream stream, void** buffers,
                      const char* opaque, size_t opaque_len) {
  std::string tmp_name;
  std::stringstream ss(opaque);

  ss >> tmp_name;

  auto it = _name_to_done_args.find(tmp_name);
  std::cout << " x2682 " << __FILE__ << ":" << __LINE__ << " in " <<__func__ <<std::endl;
  std::cout << " x2682 name_key: " << tmp_name << " rank: " << common::byteps_rank() << " waiting" << std::endl;
  // OP_REQUIRES_OK(context,  args != _name_to_done_args.end());
  ASSERTF(it != _name_to_done_args.end(), "post 2");
  auto& args = it->second;
  {
    std::unique_lock<std::mutex> lk(args.mtx);
    args.cv.wait(lk, [&args]{return args.is_done;});
  }
  _name_to_done_args.erase(it);
  std::cout << " x2682 " << __FILE__ << ":" << __LINE__ << " in " <<__func__ <<std::endl;
  std::cout << " x2682 name_key: " << tmp_name << " rank: " << common::byteps_rank() << " sync_done"<< std::endl;

}

class BytepsSyncTensorXlaOp : public ::tensorflow::XlaOpKernel {
  public:
    explicit BytepsSyncTensorXlaOp(::tensorflow::OpKernelConstruction* context) : ::tensorflow::XlaOpKernel(context) {
      context->GetAttr("input_name", &input_tensor_name);
      // OP_REQUIRES_OK(context, context->GetAttr("type", &dst_dtype_));
      // OP_REQUIRES_OK(context, DataTypeToPrimitiveType(dst_dtype_, &dst_type_));
    }
    ~BytepsSyncTensorXlaOp() override = default;

    void Compile(::tensorflow::XlaOpKernelContext* context) override {
      OP_REQUIRES_OK(context, ConvertStatus(common::CheckInitialized()));
      xla::XlaOp input_tensor = context->Input(0);
      auto input_tensor_xla_shape_or = context->InputXlaShape(0);

      auto node_name = name();
      std::string tmp_name;
      if (input_tensor_name == "default_tensor_name") {
        tmp_name = node_name;
        std::cout << " x2682 " << __FILE__ << ":" << __LINE__ << " in " <<__func__ <<std::endl;
        std::cout << " x2682 tmp_name: " << tmp_name << std::endl;
      } else {
        tmp_name = input_tensor_name;
        std::cout << " x2682 " << __FILE__ << ":" << __LINE__ << " in " <<__func__ <<std::endl;
        std::cout << " x2682 tmp_name: " << tmp_name << std::endl;
      }

      std::stringstream ss;
      ss << tmp_name;
      ss << std::endl;
      context->SetOutput(
        0, xla::CustomCall(context->builder(),
          /*call_target_name=*/"SyncTensorCustomOp",
          {input_tensor}, input_tensor_xla_shape_or.ValueOrDie(), ss.str()));

    }

  private:
     std::string input_tensor_name;
};

class BytePSSyncTensorOp : public ::tensorflow::OpKernel {
  public:
    explicit BytePSSyncTensorOp(::tensorflow::OpKernelConstruction* context) : ::tensorflow::OpKernel(context) {
      context->GetAttr("input_name", &input_tensor_name);
      // OP_REQUIRES_OK(context, context->GetAttr("type", &dst_dtype_));
      // OP_REQUIRES_OK(context, DataTypeToPrimitiveType(dst_dtype_, &dst_type_));
    }
    ~BytePSSyncTensorOp() override = default;

    void Compute(::tensorflow::OpKernelContext* context) override {
      OP_REQUIRES_OK(context, ConvertStatus(common::CheckInitialized()));
      auto input_tensor = context->input(0);

      auto node_name = name();
      std::string tmp_name;
      if (input_tensor_name == "default_tensor_name") {
        tmp_name = node_name;
        std::cout << " x2682 " << __FILE__ << ":" << __LINE__ << " in " <<__func__ <<std::endl;
        std::cout << " x2682 tmp_name: " << tmp_name << std::endl;
      } else {
        tmp_name = input_tensor_name;
        std::cout << " x2682 " << __FILE__ << ":" << __LINE__ << " in " <<__func__ <<std::endl;
        std::cout << " x2682 tmp_name: " << tmp_name << std::endl;
      }

      auto it = _name_to_done_args.find(tmp_name);
      ASSERTF(it != _name_to_done_args.end(), "pos 3");
      auto& args = it->second;
      {
        std::unique_lock<std::mutex> lk(args.mtx);
        args.cv.wait(lk, [&args]{return args.is_done;});
      }
      _name_to_done_args.erase(it);
    }

  private:
     std::string input_tensor_name;
};

REGISTER_KERNEL_BUILDER(Name("BytepsSyncTensor").Device(::tensorflow::DEVICE_GPU),
                        BytePSSyncTensorOp);
REGISTER_OP("BytepsSyncTensor")
    .Attr("T: {int32, int64, float16, float32, float64}")
    .Attr("input_name: string = 'default_tensor_name'")
    .Input("tensor: T")
    .Output("sum: T")
    .SetShapeFn([](::tensorflow::shape_inference::InferenceContext* c) {
      c->set_output(0, c->input(0));
      return ::tensorflow::Status::OK();
    });
REGISTER_XLA_OP(Name("BytepsSyncTensor"), BytepsSyncTensorXlaOp);
XLA_REGISTER_CUSTOM_CALL_TARGET(SyncTensorCustomOp, "CUDA");

void SyncAllTensorsCustomOp(CUstream stream, void** buffers,
  const char* opaque, size_t opaque_len) {
  int num;
  int seen_count = 0;
  std::vector<int> buf_sizes;
  std::string tmp_name;
  std::stringstream ss(opaque);
  int my_rank = common::byteps_rank();

  std::cout << " x2682 " << __FILE__ << ":" << __LINE__ << " in " <<__func__
            << " size_hash_table: " << _name_to_done_args.size() << std::endl;
  BPS_LOG(DEBUG, my_rank) << " x2682 got opaque: " << opaque << std::endl;
  ss >> num;
  while (ss >> tmp_name) {
    int buf_size;
    ss >> buf_size;
    if (tmp_name == "throwaway_dummy") {
      seen_count++;
      continue;
    }
    auto it = _name_to_done_args.find(tmp_name);
    std::cout << " x2682 " << __FILE__ << ":" << __LINE__ << " in " <<__func__
      << " \nname_key: " << tmp_name << " rank: " << common::byteps_rank() << " waiting" << std::endl;
    ASSERTF(it != _name_to_done_args.end(), "pos 4");
    auto& args = it->second;
    {
      std::unique_lock<std::mutex> lk(args.mtx);
      args.cv.wait(lk, [&args]{return args.is_done;});
    }
    // cudaMemcpyAsync(buffers[seen_count + num], buffers[seen_count], buf_size, cudaMemcpyDeviceToDevice, stream);
    cudaMemcpyAsync(buffers[seen_count + num], args.bps_out_buf, buf_size, cudaMemcpyDeviceToDevice, stream);
    _name_to_done_args.erase(it);
    // float i0, i1, o0, o1;
    // cudaMemcpy((void *)&i0, ((float *)buffers[count + num]), sizeof(float), cudaMemcpyDeviceToHost);
    // BPS_LOG(DEBUG, my_rank) << tmp_name << " first element: " << i0 << std::endl;
    // printMatOnGPU(tmp_name, buffers[count + num], buf_size/4);
    cudaStreamSynchronize(stream);
    // printMatOnGPU(tmp_name, args.bps_out_buf, buf_size/4);
    // BPS_LOG(DEBUG, my_rank) << tmp_name << " first element: " << *((float *)buffers[count + num]) << std::endl;
    seen_count++;
  }
  std::cout << " x2682 " << __FILE__ << ":" << __LINE__ << " in " <<__func__
    << " num: " << num << " seen_count: " << seen_count <<std::endl;
  ASSERTF(num == seen_count, "pos 5");
  BPS_LOG(DEBUG, my_rank) << "one pass ended =============================================================" << std::endl;
}

// StatusOr<std::vector<xla::Shape>> MyGetOperandShapes(
//     absl::Span<const xla::XlaOp> operands) const {
//   std::vector<xla::Shape> operand_shapes;
//   for (auto operand : operands) {
//     TF_ASSIGN_OR_RETURN(const xla::Shape* shape, GetShapePtr(operand));
//     operand_shapes.push_back(*shape);
//   }
//   return operand_shapes;
// }

/**
 * get the buffer size of the i-th input tensor
 */
int get_buf_size(::tensorflow::XlaOpKernelContext* context, int index) {
    auto xla_tensor_shape_or = context->InputXlaShape(index);
    xla::Shape tf_tensor_shape = xla_tensor_shape_or.ValueOrDie();
    int ret;

    ret = xla::ShapeUtil::ByteSizeOfPrimitiveType(tf_tensor_shape.element_type());
    for (int i = 0; i < tf_tensor_shape.rank(); i++) {
        ret *= tf_tensor_shape.dimensions(i) ;
    }

    return ret;
}

class BytePSSyncAllTensorsXlaOp : public ::tensorflow::XlaOpKernel {
  public:
    explicit BytePSSyncAllTensorsXlaOp(::tensorflow::OpKernelConstruction* context) : ::tensorflow::XlaOpKernel(context) {
      context->GetAttr("tensor_names", &tensor_names_to_sync);
    }
    ~BytePSSyncAllTensorsXlaOp() override = default;

    void Compile(::tensorflow::XlaOpKernelContext* ctx) override {
      std::vector<xla::XlaOp> values;
      std::vector<xla::XlaOp> valid_values;
      std::vector<::tensorflow::TensorShape> shapes;
      OP_REQUIRES_OK(ctx, ctx->InputList("values", &values, &shapes));
      // or, how do we get PrimitiveType or DataType from values?

      // std::vector<xla::Shape> tmp_output_shapes;
      // for (auto& tmp_shape : shapes) {
      //   tmp_output_shapes.push_back(tmp_shape.ValueOrDie());
      // }
      //
      // std::vector<xla::Shape> tmp_output_shapes = xla::GetOperandShapes(context->builder(), values);

      /**
       * there's another function in
       * tensorflow/compiler/xla/client/xla_builder.h
       *
       * // Returns the shape of the given op.
       * StatusOr<Shape> GetShape(XlaOp op) const;
       * get tensorlist shape: tensorflow/compiler/tf2xla/kernels/tensor_list_utils.h:43
       * Status GetTensorListBufferShape(xla::XlaOp list, xla::Shape* buffer_shape);
       *
       */
      const int N = values.size();
      std::vector<xla::Shape> tmp_output_shapes;
      // for (auto operand : values) {
      for (int i = 0; i < N; i++) {
        // if (tensor_names_to_sync[i] == "throwaway_dummy" ||
        //     tensor_names_to_sync[i].length() == 0) {
        //     continue;
        // }
        const xla::Shape* shape = (ctx->builder()->GetShapePtr(values[i])).ValueOrDie();
        tmp_output_shapes.push_back(*shape);
        valid_values.push_back(values[i]);
      }
      // for (int i = 0; i < N/2; i++) {
      //   tmp_output_shapes.push_back(xla::ShapeUtil::MakeShape(xla::F32, {1}));
      // }
      std::cout << "x2682 len of tmp_output_shapes: " << tmp_output_shapes.size() << std::endl;

      auto output_shapes = xla::ShapeUtil::MakeTupleShape(tmp_output_shapes);
      int num_valid_inputs = 0;
      std::cout.setf(std::ios::unitbuf);
      for (const std::string& tmp_name : tensor_names_to_sync) {
        num_valid_inputs++;
        std::cout << "x2682 got_name: " << tmp_name << " ";
        if (tmp_name == "throwaway_dummy") {
          std::cout << "counting tmp_name: " << "throwaway_dummy";
          continue;
        }
        if (tmp_name.length() == 0) {
          std::cout << "counting tmp_name: " << "throwaway_dummy";
          continue;
        }
        std::cout << "counting tmp_name: " << tmp_name;
      }
      std::cout << std::endl;
      std::cout << " x2682 " << __FILE__ << ":" << __LINE__ << " in " <<__func__
        << " num_values " << N << " num_valid_inputs: "
        << num_valid_inputs <<std::endl;
      std::stringstream ss;

      ss << num_valid_inputs;
      // for (const std::string& tmp_name : tensor_names_to_sync) {
      for (int i = 0; i < N; i++) {
          auto& tmp_name = tensor_names_to_sync[i];
          if (tmp_name == "throwaway_dummy" ||
            tmp_name.length() == 0) {
            ss << " " << "throwaway_dummy";
          } else {
            ss << " " << tmp_name;
          }

          int tmp_size = get_buf_size(ctx, i);
          ss << " " << tmp_size;
      }
      ss << std::endl;
      xla::XlaOp results = xla::CustomCall(ctx->builder(),
        /*call_target_name=*/"SyncAllTensorsCustomOp",
        valid_values, output_shapes, ss.str());

      bool is_list = IsTensorListInput(ctx, 0);
      std::cout << " x2682 " << __FILE__ << ":" << __LINE__ << " in " <<__func__
                << " is_input_0_a_list: " << is_list << std::endl;
      std::cout << " x2682 " << __FILE__ << ":" << __LINE__ << " in " <<__func__
                << " num_inputs " << ctx->num_inputs()
                << " num_outputs " << ctx->num_outputs() << std::endl;

      // method 1, runtime error
      // ctx->SetTensorListOutput(0, ctx->Input(0));
      // method 1 end
      // method 2
      // for (int i = 0; i < ctx->num_inputs(); ++i) {
      // std::cout << " x2682 " << __FILE__ << ":" << __LINE__ << " in " <<__func__
      //   << " output_num_" << i << std::endl;
      //   ctx->op_kernel_context()->set_output(i,
      //                                        ctx->op_kernel_context()->input(i));
      // std::cout << " x2682 " << __FILE__ << ":" << __LINE__ << " in " <<__func__
      //   << " output_num_" << i << " done " << std::endl;
      // }
      // method 2 end
      // method 0, correct result
      // for (int i = 0; i < num_valid_inputs; i++) {
      //   xla::XlaOp tmp_tensor = xla::GetTupleElement(results, i);
      //   ctx->SetOutput(i, tmp_tensor);
      // }
      // for (int i = num_valid_inputs; i < num_valid_inputs * 2; i++) {
      //   xla::XlaOp tmp_tensor = xla::GetTupleElement(results, i);
      //   ctx->SetOutput(i, tmp_tensor);
      // }
      // method 0 end
      // method 3
      for (int i = 0; i < ctx->num_inputs() / 2; i++) {
        xla::XlaOp tmp_tensor = xla::GetTupleElement(results, i);
        ctx->SetOutput(i, tmp_tensor);
      }
      for (int i = ctx->num_inputs() / 2; i < ctx->num_inputs(); i++) {
        std::cout << " x2682 " << __FILE__ << ":" << __LINE__ << " in " <<__func__
          << " output_num_" << i << std::endl;

        ctx->op_kernel_context()->set_output(i,
          ctx->op_kernel_context()->input(i));

        std::cout << " x2682 " << __FILE__ << ":" << __LINE__ << " in " <<__func__
          << " output_num_" << i << " done " << std::endl;
      }
    }

  private:
    std::vector<std::string> tensor_names_to_sync;
};

REGISTER_OP("BytepsSyncAllTensors")
    .Input("values: N * T")
    .Output("sum: N * T")
    .Attr("T: {int32, int64, float16, float32, float64}")
    .Attr("N: int >= 1")
    .Attr("tensor_names: list(string)")
    .SetShapeFn([](::tensorflow::shape_inference::InferenceContext* c) {
      // c->set_output(0, c->input(0));
      // return ::tensorflow::Status::OK();
      const int n = c->num_inputs();
      for (int i = 0; i < n; i++) {
        c->set_output(i, c->input(i));
      }
      return ::tensorflow::Status::OK();
    });
REGISTER_XLA_OP(Name("BytepsSyncAllTensors"), BytePSSyncAllTensorsXlaOp);
XLA_REGISTER_CUSTOM_CALL_TARGET(SyncAllTensorsCustomOp, "CUDA");
////////////////////////////////////////////////////////////////////////////
//for debugging

void PrintTensorsCustomOp(CUstream stream, void** buffers,
  const char* opaque, size_t opaque_len)
{
  int num;
  int count = 0;
  std::vector<int> buf_sizes;
  std::string tmp_name;
  std::stringstream ss(opaque);
  int my_rank = common::byteps_rank();

  BPS_LOG(DEBUG, my_rank) << " x2682 got opaque: " << opaque << std::endl;
  ss >> num;
  while (ss >> tmp_name) {
    int buf_size;
    ss >> buf_size;
    cudaMemcpyAsync(buffers[count + num], buffers[count], buf_size, cudaMemcpyDeviceToDevice, stream);
    // cudaStreamSynchronize(stream);
    printMatOnGPU(tmp_name, buffers[count], buf_size/4);
    count++;
  }
  std::cout << " x2682 " << __FILE__ << ":" << __LINE__ << " in " <<__func__
    << " num: " << num << " count: " << count <<std::endl;
  ASSERTF(num == count, "pos 5");
  BPS_LOG(DEBUG, my_rank) << "one pass ended =============================================================" << std::endl;
}

class BytePSPrintTensorsXlaOp : public ::tensorflow::XlaOpKernel
{
  public:
    explicit BytePSPrintTensorsXlaOp(::tensorflow::OpKernelConstruction* context) : ::tensorflow::XlaOpKernel(context) {
      context->GetAttr("tensor_names", &tensor_names_to_print);
    }
    ~BytePSPrintTensorsXlaOp() override = default;

    void Compile(::tensorflow::XlaOpKernelContext* ctx) override {
      std::vector<xla::XlaOp> values;
      std::vector<xla::XlaOp> valid_values;
      std::vector<::tensorflow::TensorShape> shapes;
      OP_REQUIRES_OK(ctx, ctx->InputList("values", &values, &shapes));
      const int N = values.size();
      std::vector<xla::Shape> tmp_output_shapes;
      // for (auto operand : values) {
      for (int i = 0; i < N; i++) {
        const xla::Shape* shape = (ctx->builder()->GetShapePtr(values[i])).ValueOrDie();
        tmp_output_shapes.push_back(*shape);
      }

      auto output_shapes = xla::ShapeUtil::MakeTupleShape(tmp_output_shapes);

      std::stringstream ss;
      ss << N;
      for (int i = 0; i < N; i++) {
          auto& tmp_name = tensor_names_to_print[i];
          int tmp_size = get_buf_size(ctx, i);
          ss << " " << tmp_name;
          ss << " " << tmp_size;
      }
      ss << std::endl;
      xla::XlaOp results = xla::CustomCall(ctx->builder(),
        /*call_target_name=*/"PrintTensorsCustomOp",
        valid_values, output_shapes, ss.str());

      for (int i = 0; i < N; i++) {
        xla::XlaOp tmp_tensor = xla::GetTupleElement(results, i);
        ctx->SetOutput(i, tmp_tensor);
      }
    }

  private:
    std::vector<std::string> tensor_names_to_print;
};

REGISTER_OP("BytepsPrintTensors")
    .Input("values: N * T")
    .Output("sum: N * T")
    .Attr("T: {int32, int64, float16, float32, float64}")
    .Attr("N: int >= 1")
    .Attr("tensor_names: list(string)")
    .SetShapeFn([](::tensorflow::shape_inference::InferenceContext* c) {
      c->set_output(0, c->input(0));
      return ::tensorflow::Status::OK();
    });
REGISTER_XLA_OP(Name("BytepsPrintTensors"), BytePSPrintTensorsXlaOp);
XLA_REGISTER_CUSTOM_CALL_TARGET(PrintTensorsCustomOp, "CUDA");

}  // namespace tensorflow
}  // namespace byteps

#if 0
// exampel to serialize and deserialize strings  and pointers
#include <iostream>
#include <sstream>

using namespace std;

int main()
{
    cout<<"Hello World";
    int a = 5678;
    int *a_ptr = &a;
//    string mystr = to_string(static_cast<unsigned long>(a_ptr));
//    cout<< "mystr" << mystr << endl;
    string opaque;
    stringstream ss(opaque);

    ss << "aabbcc" << " " << a << " " << a_ptr << endl;
    cout << "string is " << ss.str() <<endl;

    string tmp_str;
    int b = 0;
    int *b_ptr = NULL;
    string  ptr_str;

    ss >> tmp_str;;
    ss >> b;
    ss >> hex >> ptr_str;
    b_ptr = (int *) stoul(ptr_str, nullptr, 0);
    cout << "val of b is " << *b_ptr << endl;



    return 0;
}
#endif
