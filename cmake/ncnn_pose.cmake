# Extra ncnn layers for YOLO11-pose (WAVE_BUILD_POSE=ON).
# Merged into NCNN_ENABLED_LAYERS by cmake/ncnn.cmake.

set(NCNN_POSE_EXTRA_LAYERS
    convolution
    convolutiondepthwise
    pooling
    concat
    split
    slice
    reshape
    permute
    sigmoid
    softmax
    swish
    hardswish
    mish
    binaryop
    unaryop
    eltwise
    scale
    dropout
    batchnorm
    interp
    deconvolution
    reorg
    pixelshuffle
    gelu
)
