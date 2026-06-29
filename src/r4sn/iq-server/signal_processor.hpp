#pragma once

#include <vector>

#include <r4sn/iq_protocol.h>

#include "cube_mmap.hpp"
#include "steering_cache.hpp"

class SignalProcessor
{
public:
    iq::IqResponse processIq(const RangeCube& cube, const iq::IqRequestMsg& msg) const;
    iq::RdmResponse processRdm(const DopplerCube& cube, const iq::RdmRequestMsg& msg) const;

private:
    mutable SteeringCache m_steerCache;
};
