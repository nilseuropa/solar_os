#include "solar_os_inference_types_v1.h"
size_t solar_os_tensor_dtype_bytes(solar_os_tensor_dtype_t dtype)
{
    switch(dtype) {
    case SOLAR_OS_TENSOR_INT8:case SOLAR_OS_TENSOR_UINT8:case SOLAR_OS_TENSOR_BOOL:return 1;
    case SOLAR_OS_TENSOR_INT16:case SOLAR_OS_TENSOR_UINT16:case SOLAR_OS_TENSOR_FLOAT16:return 2;
    case SOLAR_OS_TENSOR_INT32:case SOLAR_OS_TENSOR_UINT32:case SOLAR_OS_TENSOR_FLOAT32:return 4;
    case SOLAR_OS_TENSOR_INT64:case SOLAR_OS_TENSOR_UINT64:case SOLAR_OS_TENSOR_FLOAT64:return 8;
    default:return 0;
    }
}
