import tensorflow as tf
import byteps.tensorflow as bps
import os

SEED = 0
os.environ['PYTHONHASHSEED']=str(SEED)
tf.random.set_seed(SEED)
os.environ['TF_DETERMINISTIC_OPS'] = '1'

os.environ["TF_CPP_MIN_LOG_LEVEL"] = "3"
bps.init()

# BytePS: pin GPU to be used to process local rank (one GPU per process)
gpus = tf.config.experimental.list_physical_devices('GPU')
for gpu in gpus:
    tf.config.experimental.set_memory_growth(gpu, True)
if gpus:
    tf.config.experimental.set_visible_devices(gpus[bps.local_rank()], 'GPU')

# Before launching, need to fist download the dataset to ~/.keras/datasets
(mnist_images, mnist_labels), _ = \
    tf.keras.datasets.mnist.load_data(path='mnist-%d.npz' % bps.rank())

dataset = tf.data.Dataset.from_tensor_slices(
    (tf.cast(mnist_images[..., tf.newaxis] / 255.0, tf.float32),
             tf.cast(mnist_labels, tf.int64))
)
dataset = dataset.repeat().shuffle(10000).batch(128)

mnist_model = tf.keras.Sequential([
    tf.keras.layers.Conv2D(32, [3, 3], activation='relu'),
    tf.keras.layers.Conv2D(64, [3, 3], activation='relu'),
    tf.keras.layers.MaxPooling2D(pool_size=(2, 2)),
    tf.keras.layers.Dropout(0.25),
    tf.keras.layers.Flatten(),
    tf.keras.layers.Dense(128, activation='relu'),
    tf.keras.layers.Dropout(0.5),
    tf.keras.layers.Dense(10, activation='softmax')
])
loss = tf.losses.SparseCategoricalCrossentropy()

opt = tf.optimizers.Adam(0.001 * bps.size())

checkpoint_dir = './checkpoints'
checkpoint = tf.train.Checkpoint(model=mnist_model, optimizer=opt)


# @tf.function()
@tf.function(experimental_compile=True)
def training_step(images, labels, first_batch):
    with tf.GradientTape() as tape:
        probs = mnist_model(images, training=True)
        loss_value = loss(labels, probs)

    tape = bps.DistributedGradientTape(tape)

    grads = tape.gradient(loss_value, mnist_model.trainable_variables)
    if first_batch:
        grads = bps._print_tensors(grads, [aa.name for aa in grads])
    # add another call to sync all tensors.
    opt.apply_gradients(zip(grads, mnist_model.trainable_variables))

    # Note: broadcast should be done after the first gradient step to ensure optimizer
    # initialization.
    # if first_batch:
    #     bps.broadcast_variables(mnist_model.variables, root_rank=0)
    #     # this is the culprit
    #     bps.broadcast_variables(opt.variables(), root_rank=0)

    return loss_value

LOG_DIR = 'logdir'
tf.profiler.experimental.start(LOG_DIR)
# BytePS: adjust number of steps based on number of GPUs.
# for batch, (images, labels) in enumerate(dataset.take(10000 // bps.size())):
# for batch, (images, labels) in enumerate(dataset.take(100 // bps.size())):
# for batch, (images, labels) in enumerate(dataset.take(bps.size() * 2 * 50 // bps.size())):
for batch, (images, labels) in enumerate(dataset.take(bps.size() * 10 // bps.size())):
    # with tf.profiler.experimental.Trace("Train", step_num=batch):
    loss_value = training_step(images, labels, batch == 0)
    if batch == 0:
        bps.broadcast_variables(mnist_model.variables, root_rank=0)
        # this is the culprit
        bps.broadcast_variables(opt.variables(), root_rank=0)
        tf.print("step ", batch, output_stream="file:///opt/tiger/byteps-xla/example/tensorflow/print" + str(bps.rank()))
        tf.print("mnist-model_vars", mnist_model.variables, output_stream="file:///opt/tiger/byteps-xla/example/tensorflow/print" + str(bps.rank()))
        tf.print("opt_vars", opt.variables(), output_stream="file:///opt/tiger/byteps-xla/example/tensorflow/print" + str(bps.rank()))

    # if batch % 10 == 0 and bps.local_rank() == 0:
    if bps.local_rank() == 0:
        print('Step #%d\tLoss: %.6f' % (batch, loss_value))
tf.profiler.experimental.stop()

tb_callback = tf.keras.callbacks.TensorBoard(LOG_DIR)
tb_callback.set_model(mnist_model) # Writes the graph to tensorboard summaries using an internal file writer
# if bps.rank() == 0:
#     checkpoint.save(checkpoint_dir)
