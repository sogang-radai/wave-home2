#pragma once

#include <array>
#include <cmath>
#include <complex>
#include <cstddef>

#include "cube_mmap.hpp"
#include "radar_params.hpp"
#include "va_geometry.h"

class SteeringCache
{
public:
    static constexpr size_t kSlotCount = 16;

    const std::array<std::complex<float>, kVaCount>& get(float azimuth_rad, float elevation_rad) const
    {
        if (m_lastHit < kSlotCount
            && m_slots[m_lastHit].valid
            && m_slots[m_lastHit].azimuthRad == azimuth_rad
            && m_slots[m_lastHit].elevationRad == elevation_rad)
        {
            return m_slots[m_lastHit].weights;
        }

        for (size_t i = 0; i < kSlotCount; ++i)
        {
            if (m_slots[i].valid
                && m_slots[i].azimuthRad == azimuth_rad
                && m_slots[i].elevationRad == elevation_rad)
            {
                m_lastHit = i;
                return m_slots[i].weights;
            }
        }

        return insert(azimuth_rad, elevation_rad);
    }

private:
    struct Slot
    {
        float azimuthRad = 0.0f;
        float elevationRad = 0.0f;
        std::array<std::complex<float>, kVaCount> weights{};
        bool valid = false;
    };

    const std::array<std::complex<float>, kVaCount>& insert(float azimuth_rad, float elevation_rad) const
    {
        const size_t idx = m_roundRobin;
        m_roundRobin = (m_roundRobin + 1) % kSlotCount;
        build(azimuth_rad, elevation_rad, m_slots[idx].weights);
        m_slots[idx].azimuthRad = azimuth_rad;
        m_slots[idx].elevationRad = elevation_rad;
        m_slots[idx].valid = true;
        m_lastHit = idx;
        return m_slots[idx].weights;
    }

    static void build(
        float azimuth_rad,
        float elevation_rad,
        std::array<std::complex<float>, kVaCount>& weights)
    {
        const float sin_theta = std::sin(azimuth_rad);
        const float cos_phi = std::cos(elevation_rad);
        const float sin_phi = std::sin(elevation_rad);
        const float u = sin_theta * cos_phi;

        for (size_t va_idx = 0; va_idx < kVaCount; ++va_idx)
        {
            const float x_m = kVaXHalf[va_idx] * kHalfLambda;
            const float y_m = kVaYHalf[va_idx] * kHalfLambda;
            const float phase = -6.28318530718f / kWavelength * (x_m * u + y_m * sin_phi);
#if defined(__GNUC__) || defined(__clang__)
            float sin_p = 0.0f;
            float cos_p = 0.0f;
            sincosf(phase, &sin_p, &cos_p);
            weights[va_idx] = {cos_p, sin_p};
#else
            weights[va_idx] = {std::cos(phase), std::sin(phase)};
#endif
        }
    }

    mutable Slot m_slots[kSlotCount]{};
    mutable size_t m_roundRobin = 0;
    mutable size_t m_lastHit = 0;
};
