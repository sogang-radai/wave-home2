#pragma once

#include <vector>
#include <r4sn/iq_protocol.h>
#include "cube_mmap.hpp"

namespace r4sn
{
    class SignalProcessor
    {
    public:
        std::vector<iq::ComplexF32> processTarget(const RangeCube& cube, const iq::TargetSpec& target) const;
        std::vector<iq::ComplexF32> processRequest(
            const RangeCube& cube,
            const std::vector<iq::TargetSpec>& targets,
            iq::VaCombineMode va_mode = iq::VaCombineMode::Beamform,
            uint8_t tile = 0,
            uint8_t sub_ant = 0) const;
        iq::RdmResponse processRdm(const DopplerCube& doppler_cube, const iq::RdmRequestSpec& spec) const;
    };
}  // namespace r4sn
