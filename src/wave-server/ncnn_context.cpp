#include "ncnn_context.h"

#include <net.h>

WH_NAMESPACE_BEGIN

NcnnContext::NcnnContext() :
    m_ready(false)
{
    ncnn::Net net;
    m_ready = true;
}

WH_NAMESPACE_END
