# Minimal NCNN for 1D CNN, LSTM, and PointNet inference.

set(NCNN_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(NCNN_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(NCNN_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(NCNN_BUILD_BENCHMARK OFF CACHE BOOL "" FORCE)
set(NCNN_INSTALL_SDK OFF CACHE BOOL "" FORCE)
set(NCNN_VULKAN OFF CACHE BOOL "" FORCE)
set(NCNN_SIMPLEVK OFF CACHE BOOL "" FORCE)

set(NCNN_BENCHMARK OFF CACHE BOOL "" FORCE)
set(NCNN_C_API OFF CACHE BOOL "" FORCE)
set(NCNN_PLATFORM_API OFF CACHE BOOL "" FORCE)
set(NCNN_PIXEL OFF CACHE BOOL "" FORCE)
set(NCNN_PIXEL_ROTATE OFF CACHE BOOL "" FORCE)
set(NCNN_PIXEL_AFFINE OFF CACHE BOOL "" FORCE)
set(NCNN_PIXEL_DRAWING OFF CACHE BOOL "" FORCE)
set(NCNN_INT8 OFF CACHE BOOL "" FORCE)
set(NCNN_BF16 OFF CACHE BOOL "" FORCE)
set(NCNN_PYTHON OFF CACHE BOOL "" FORCE)
set(NCNN_RUNTIME_CPU ON CACHE BOOL "" FORCE)
set(NCNN_OPENMP ON CACHE BOOL "" FORCE)

if(WAVE_ARCH_AARCH64)
    set(NCNN_VFPV4 ON CACHE BOOL "" FORCE)
    set(NCNN_ARM82 ON CACHE BOOL "" FORCE)
endif()

set(NCNN_ENABLED_LAYERS
    input convolution1d batchnorm relu pooling1d pooling reduction
    dropout innerproduct permute reshape flatten lstm bias scale padding
    concat split slice softmax binaryop unaryop noop memorydata clip cast
)

set(NCNN_ALL_LAYERS
    absval argmax batchnorm bias bnll concat convolution crop deconvolution
    dropout eltwise elu embed exp flatten innerproduct input log lrn memorydata
    mvn pooling power prelu proposal reduction relu reshape roipooling scale
    sigmoid slice softmax split spp tanh threshold tile rnn lstm binaryop unaryop
    convolutiondepthwise padding squeeze expanddims normalize permute priorbox
    detectionoutput interp deconvolutiondepthwise shufflechannel instancenorm clip
    reorg yolodetectionoutput quantize dequantize yolov3detectionoutput psroipooling
    roialign packing requantize cast hardsigmoid selu hardswish noop pixelshuffle
    deepcopy mish statisticspooling swish gemm groupnorm layernorm softplus gru
    multiheadattention gelu convolution1d pooling1d convolutiondepthwise1d
    convolution3d convolutiondepthwise3d pooling3d matmul deconvolution1d
    deconvolutiondepthwise1d deconvolution3d deconvolutiondepthwise3d einsum
    deformableconv2d glu fold unfold gridsample cumulativesum copyto erf diag celu
    shrink rmsnorm spectrogram inversespectrogram flip sdpa rotaryembed
)

foreach(layer IN LISTS NCNN_ALL_LAYERS)
    if(layer IN_LIST NCNN_ENABLED_LAYERS)
        set(WITH_LAYER_${layer} ON CACHE BOOL "" FORCE)
    else()
        set(WITH_LAYER_${layer} OFF CACHE BOOL "" FORCE)
    endif()
endforeach()

set(WITH_LAYER_spp OFF CACHE BOOL "" FORCE)

add_subdirectory("${CMAKE_SOURCE_DIR}/thirdparty/ncnn" "${CMAKE_BINARY_DIR}/thirdparty/ncnn")
