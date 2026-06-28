#pragma once

#include "whdefs.h"

#include <string>

WH_NAMESPACE_BEGIN

class NcnnContext
{
public:
    NcnnContext();

    bool isReady() const { return m_ready; }

private:
    bool m_ready;
};

WH_NAMESPACE_END
