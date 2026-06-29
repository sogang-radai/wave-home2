#include "signal_processor.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <limits>
#include <vector>

#include "radar_params.hpp"

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define IQ_SERVER_HAVE_NEON 1
#endif

namespace
{
    inline bool chirpEnabled(const iq::ChirpSelectMode mode, const uint64_t mask, size_t chirp)
    {
        if (mode == iq::ChirpSelectMode::All)
            return true;
        return (mask >> chirp) & 1ull;
    }

    inline size_t chirpOutputCount(const iq::ChirpMode mode)
    {
        return mode == iq::ChirpMode::Array ? kChirpCount : 1;
    }

    inline float complexNormSq(const std::complex<float>& z)
    {
        return z.real() * z.real() + z.imag() * z.imag();
    }

    inline std::complex<float> readVaSample(
        const int16_t (&plane)[kSubAntCount][2],
        size_t sub_ant)
    {
        return {static_cast<float>(plane[sub_ant][1]), static_cast<float>(plane[sub_ant][0])};
    }

    template <typename Msg>
    bool isVaSelected(
        const iq::VaSelectMode select_mode,
        uint8_t tile,
        uint8_t sub_ant,
        uint8_t req_tile,
        uint8_t req_sub_ant,
        const Msg& msg)
    {
        switch (select_mode)
        {
        case iq::VaSelectMode::Single:
            return tile == req_tile && sub_ant == req_sub_ant;
        case iq::VaSelectMode::Mask:
            if (!msg.va_masks)
                return false;
            return (msg.va_masks->at(tile) >> sub_ant) & 1u;
        case iq::VaSelectMode::List:
            for (const auto& item : msg.va_list)
            {
                if (item.tile == tile && item.sub_ant == sub_ant)
                    return true;
            }
            return false;
        }
        return true;
    }

#ifdef IQ_SERVER_HAVE_NEON
    inline std::complex<float> dotSteerNeon(
        const std::array<std::complex<float>, kVaCount>& accum,
        const std::array<std::complex<float>, kVaCount>& steer,
        float scale)
    {
        float32x4_t sum_re = vdupq_n_f32(0.0f);
        float32x4_t sum_im = vdupq_n_f32(0.0f);
        size_t va = 0;
        for (; va + 4 <= kVaCount; va += 4)
        {
            const float ar[4] = {
                accum[va].real(), accum[va + 1].real(), accum[va + 2].real(), accum[va + 3].real()};
            const float ai[4] = {
                accum[va].imag(), accum[va + 1].imag(), accum[va + 2].imag(), accum[va + 3].imag()};
            const float wr[4] = {
                steer[va].real(), steer[va + 1].real(), steer[va + 2].real(), steer[va + 3].real()};
            const float wi[4] = {
                steer[va].imag(), steer[va + 1].imag(), steer[va + 2].imag(), steer[va + 3].imag()};
            const float32x4_t a_re = vld1q_f32(ar);
            const float32x4_t a_im = vld1q_f32(ai);
            const float32x4_t w_re = vld1q_f32(wr);
            const float32x4_t w_im = vld1q_f32(wi);
            sum_re = vaddq_f32(sum_re, vsubq_f32(vmulq_f32(a_re, w_re), vmulq_f32(a_im, w_im)));
            sum_im = vaddq_f32(sum_im, vaddq_f32(vmulq_f32(a_re, w_im), vmulq_f32(a_im, w_re)));
        }
        float re = vgetq_lane_f32(sum_re, 0) + vgetq_lane_f32(sum_re, 1)
            + vgetq_lane_f32(sum_re, 2) + vgetq_lane_f32(sum_re, 3);
        float im = vgetq_lane_f32(sum_im, 0) + vgetq_lane_f32(sum_im, 1)
            + vgetq_lane_f32(sum_im, 2) + vgetq_lane_f32(sum_im, 3);
        for (; va < kVaCount; ++va)
        {
            re += accum[va].real() * steer[va].real() - accum[va].imag() * steer[va].imag();
            im += accum[va].real() * steer[va].imag() + accum[va].imag() * steer[va].real();
        }
        return {re * scale, im * scale};
    }
#endif

    inline std::complex<float> dotSteerAccum(
        const std::array<std::complex<float>, kVaCount>& accum,
        const std::array<std::complex<float>, kVaCount>& steer,
        float scale)
    {
#ifdef IQ_SERVER_HAVE_NEON
        return dotSteerNeon(accum, steer, scale);
#else
        std::complex<float> sum{0.0f, 0.0f};
        for (size_t va = 0; va < kVaCount; ++va)
            sum += accum[va] * steer[va];
        return sum * scale;
#endif
    }

    template <typename Cube, typename Msg>
    inline void accumulateVaChirps(
        const Cube& cube,
        int range_bin,
        const Msg& msg,
        const iq::IqRequest& req,
        std::array<std::complex<float>, kVaCount>& accum)
    {
        accum.fill({0.0f, 0.0f});
        const uint64_t chirp_mask = msg.chirp_mask.value_or(iq::kChirpMaskAll);
        for (size_t chirp = 0; chirp < kChirpCount; ++chirp)
        {
            if (!chirpEnabled(req.chirp_select_mode, chirp_mask, chirp))
                continue;

            for (size_t tile = 0; tile < kTileCount; ++tile)
            {
                const auto& plane = cube[tile][range_bin][chirp];
                for (size_t sub_ant = 0; sub_ant < kSubAntCount; ++sub_ant)
                {
                    if (req.va_combine_mode == iq::VaCombineMode::MultiVa
                        && !isVaSelected(req.va_select_mode, static_cast<uint8_t>(tile),
                            static_cast<uint8_t>(sub_ant), req.tile, req.sub_ant, msg))
                    {
                        continue;
                    }

                    const size_t va_idx = tile * kSubAntCount + sub_ant;
                    accum[va_idx] += readVaSample(plane, sub_ant);
                }
            }
        }
    }

#ifdef IQ_SERVER_HAVE_NEON
    template <typename Cube>
    inline std::complex<float> beamformAtNeon(
        const Cube& cube,
        const std::array<std::complex<float>, kVaCount>& steer,
        int range_bin,
        size_t slow_idx)
    {
        float32x4_t sum_re = vdupq_n_f32(0.0f);
        float32x4_t sum_im = vdupq_n_f32(0.0f);
        for (size_t tile = 0; tile < kTileCount; ++tile)
        {
            const auto& plane = cube[tile][range_bin][slow_idx];
            for (size_t sub_ant = 0; sub_ant < kSubAntCount; sub_ant += 4)
            {
                const size_t va_idx = tile * kSubAntCount + sub_ant;
                const float sr[4] = {
                    static_cast<float>(plane[sub_ant + 0][1]),
                    static_cast<float>(plane[sub_ant + 1][1]),
                    static_cast<float>(plane[sub_ant + 2][1]),
                    static_cast<float>(plane[sub_ant + 3][1]),
                };
                const float si[4] = {
                    static_cast<float>(plane[sub_ant + 0][0]),
                    static_cast<float>(plane[sub_ant + 1][0]),
                    static_cast<float>(plane[sub_ant + 2][0]),
                    static_cast<float>(plane[sub_ant + 3][0]),
                };
                const float wr[4] = {
                    steer[va_idx + 0].real(),
                    steer[va_idx + 1].real(),
                    steer[va_idx + 2].real(),
                    steer[va_idx + 3].real(),
                };
                const float wi[4] = {
                    steer[va_idx + 0].imag(),
                    steer[va_idx + 1].imag(),
                    steer[va_idx + 2].imag(),
                    steer[va_idx + 3].imag(),
                };
                const float32x4_t s_re = vld1q_f32(sr);
                const float32x4_t s_im = vld1q_f32(si);
                const float32x4_t w_re = vld1q_f32(wr);
                const float32x4_t w_im = vld1q_f32(wi);
                sum_re = vaddq_f32(sum_re, vsubq_f32(vmulq_f32(s_re, w_re), vmulq_f32(s_im, w_im)));
                sum_im = vaddq_f32(sum_im, vaddq_f32(vmulq_f32(s_re, w_im), vmulq_f32(s_im, w_re)));
            }
        }
        const float re = vgetq_lane_f32(sum_re, 0) + vgetq_lane_f32(sum_re, 1)
            + vgetq_lane_f32(sum_re, 2) + vgetq_lane_f32(sum_re, 3);
        const float im = vgetq_lane_f32(sum_im, 0) + vgetq_lane_f32(sum_im, 1)
            + vgetq_lane_f32(sum_im, 2) + vgetq_lane_f32(sum_im, 3);
        return {re, im};
    }
#endif

    template <typename Cube>
    inline std::complex<float> beamformAt(
        const Cube& cube,
        const std::array<std::complex<float>, kVaCount>& steer,
        int range_bin,
        size_t slow_idx)
    {
#ifdef IQ_SERVER_HAVE_NEON
        return beamformAtNeon(cube, steer, range_bin, slow_idx);
#else
        std::complex<float> sum{0.0f, 0.0f};
        for (size_t tile = 0; tile < kTileCount; ++tile)
        {
            const auto& plane = cube[tile][range_bin][slow_idx];
            for (size_t sub_ant = 0; sub_ant < kSubAntCount; ++sub_ant)
            {
                const size_t va_idx = tile * kSubAntCount + sub_ant;
                sum += readVaSample(plane, sub_ant) * steer[va_idx];
            }
        }
        return sum;
#endif
    }

    template <typename Cube, typename Msg>
    inline std::complex<float> beamformAverageChirps(
        const Cube& cube,
        const std::array<std::complex<float>, kVaCount>& steer,
        int range_bin,
        const Msg& msg,
        const iq::IqRequest& req)
    {
        std::array<std::complex<float>, kVaCount> accum{};
        accumulateVaChirps(cube, range_bin, msg, req, accum);
        constexpr float kInvChirps = 1.0f / static_cast<float>(kChirpCount);
        return dotSteerAccum(accum, steer, kInvChirps);
    }

    template <typename Cube, typename Msg>
    inline std::complex<float> beamformMaxAbsChirps(
        const Cube& cube,
        const std::array<std::complex<float>, kVaCount>& steer,
        int range_bin,
        const Msg& msg,
        const iq::RdmRequest& req)
    {
        std::complex<float> best{0.0f, 0.0f};
        float best_norm = 0.0f;
        const uint64_t chirp_mask = msg.chirp_mask.value_or(iq::kChirpMaskAll);
        for (size_t chirp = 0; chirp < kChirpCount; ++chirp)
        {
            if (!chirpEnabled(req.chirp_select_mode, chirp_mask, chirp))
                continue;

            const auto value = beamformAt(cube, steer, range_bin, chirp);
            const float norm = complexNormSq(value);
            if (norm > best_norm)
            {
                best_norm = norm;
                best = value;
            }
        }
        return best;
    }

    template <typename Cube, typename Msg>
    inline std::complex<float> averageAllAt(
        const Cube& cube,
        int range_bin,
        size_t slow_idx,
        const Msg& msg,
        const iq::VaSelectMode select_mode,
        uint8_t tile,
        uint8_t sub_ant)
    {
        std::complex<float> sum{0.0f, 0.0f};
        size_t count = 0;
        for (size_t t = 0; t < kTileCount; ++t)
        {
            const auto& plane = cube[t][range_bin][slow_idx];
            for (size_t s = 0; s < kSubAntCount; ++s)
            {
                if (!isVaSelected(select_mode, static_cast<uint8_t>(t), static_cast<uint8_t>(s),
                        tile, sub_ant, msg))
                {
                    continue;
                }
                sum += readVaSample(plane, s);
                ++count;
            }
        }
        if (count == 0)
            return {0.0f, 0.0f};
        return sum / static_cast<float>(count);
    }

    template <typename Cube, typename Msg>
    inline std::complex<float> averageAllAverageChirps(
        const Cube& cube,
        int range_bin,
        const Msg& msg,
        const iq::RdmRequest& req)
    {
        std::complex<float> sum{0.0f, 0.0f};
        size_t count = 0;
        const uint64_t chirp_mask = msg.chirp_mask.value_or(iq::kChirpMaskAll);
        for (size_t chirp = 0; chirp < kChirpCount; ++chirp)
        {
            if (!chirpEnabled(req.chirp_select_mode, chirp_mask, chirp))
                continue;

            for (size_t tile = 0; tile < kTileCount; ++tile)
            {
                const auto& plane = cube[tile][range_bin][chirp];
                for (size_t sub_ant = 0; sub_ant < kSubAntCount; ++sub_ant)
                {
                    if (!isVaSelected(req.va_select_mode, static_cast<uint8_t>(tile),
                            static_cast<uint8_t>(sub_ant), req.tile, req.sub_ant, msg))
                    {
                        continue;
                    }
                    sum += readVaSample(plane, sub_ant);
                    ++count;
                }
            }
        }
        if (count == 0)
            return {0.0f, 0.0f};
        return sum / static_cast<float>(count);
    }

    template <typename Cube>
    inline std::complex<float> singleVaAt(
        const Cube& cube,
        uint8_t tile,
        int range_bin,
        size_t slow_idx,
        uint8_t sub_ant)
    {
        return readVaSample(cube[tile][range_bin][slow_idx], sub_ant);
    }

    template <typename Cube, typename Msg>
    inline std::complex<float> singleVaAverageChirps(
        const Cube& cube,
        uint8_t tile,
        int range_bin,
        uint8_t sub_ant,
        const Msg& msg,
        const iq::ChirpSelectMode select_mode)
    {
        std::complex<float> sum{0.0f, 0.0f};
        size_t count = 0;
        const uint64_t chirp_mask = msg.chirp_mask.value_or(iq::kChirpMaskAll);
        for (size_t chirp = 0; chirp < kChirpCount; ++chirp)
        {
            if (!chirpEnabled(select_mode, chirp_mask, chirp))
                continue;
            sum += readVaSample(cube[tile][range_bin][chirp], sub_ant);
            ++count;
        }
        if (count == 0)
            return {0.0f, 0.0f};
        return sum / static_cast<float>(count);
    }

    template <typename Cube, typename Req, typename Msg>
    inline std::complex<float> extractCombinedSample(
        const Cube& cube,
        const Req& req,
        const Msg& msg,
        const std::array<std::complex<float>, kVaCount>* steer,
        int range_bin,
        iq::ChirpMode chirp_mode)
    {
        switch (chirp_mode)
        {
        case iq::ChirpMode::Average:
            switch (req.va_combine_mode)
            {
            case iq::VaCombineMode::MultiVa:
                return singleVaAverageChirps(
                    cube, req.tile, range_bin, req.sub_ant, msg, req.chirp_select_mode);
            case iq::VaCombineMode::AverageAll:
                if constexpr (std::is_same_v<Req, iq::RdmRequest>)
                    return averageAllAverageChirps(cube, range_bin, msg, req);
                else
                {
                    iq::RdmRequest shim{};
                    shim.chirp_select_mode = req.chirp_select_mode;
                    shim.va_select_mode = req.va_select_mode;
                    shim.tile = req.tile;
                    shim.sub_ant = req.sub_ant;
                    return averageAllAverageChirps(cube, range_bin, msg, shim);
                }
            case iq::VaCombineMode::Beamform:
                if constexpr (std::is_same_v<Req, iq::IqRequest>)
                    return beamformAverageChirps(cube, *steer, range_bin, msg, req);
                else
                {
                    iq::IqRequest shim{};
                    shim.chirp_select_mode = req.chirp_select_mode;
                    shim.va_select_mode = req.va_select_mode;
                    shim.tile = req.tile;
                    shim.sub_ant = req.sub_ant;
                    return beamformAverageChirps(cube, *steer, range_bin, msg, shim);
                }
            }
            break;
        case iq::ChirpMode::MaxAbs:
            switch (req.va_combine_mode)
            {
            case iq::VaCombineMode::MultiVa:
            {
                std::complex<float> best{0.0f, 0.0f};
                float best_norm = 0.0f;
                const uint64_t chirp_mask = msg.chirp_mask.value_or(iq::kChirpMaskAll);
                for (size_t chirp = 0; chirp < kChirpCount; ++chirp)
                {
                    if (!chirpEnabled(req.chirp_select_mode, chirp_mask, chirp))
                        continue;
                    const auto value = singleVaAt(cube, req.tile, range_bin, chirp, req.sub_ant);
                    const float norm = complexNormSq(value);
                    if (norm > best_norm)
                    {
                        best_norm = norm;
                        best = value;
                    }
                }
                return best;
            }
            case iq::VaCombineMode::AverageAll:
            {
                std::complex<float> best{0.0f, 0.0f};
                float best_norm = 0.0f;
                const uint64_t chirp_mask = msg.chirp_mask.value_or(iq::kChirpMaskAll);
                for (size_t chirp = 0; chirp < kChirpCount; ++chirp)
                {
                    if (!chirpEnabled(req.chirp_select_mode, chirp_mask, chirp))
                        continue;
                    const auto value = averageAllAt(
                        cube, range_bin, chirp, msg, req.va_select_mode, req.tile, req.sub_ant);
                    const float norm = complexNormSq(value);
                    if (norm > best_norm)
                    {
                        best_norm = norm;
                        best = value;
                    }
                }
                return best;
            }
            case iq::VaCombineMode::Beamform:
                if constexpr (std::is_same_v<Req, iq::RdmRequest>)
                    return beamformMaxAbsChirps(cube, *steer, range_bin, msg, req);
                else
                {
                    iq::RdmRequest shim{};
                    shim.chirp_select_mode = req.chirp_select_mode;
                    return beamformMaxAbsChirps(cube, *steer, range_bin, msg, shim);
                }
            }
            break;
        case iq::ChirpMode::Array:
            break;
        }
        return {0.0f, 0.0f};
    }

    std::vector<iq::ComplexF32> reduceChirps(
        const std::array<std::complex<float>, kChirpCount>& chirps,
        iq::ChirpMode mode)
    {
        switch (mode)
        {
        case iq::ChirpMode::Array:
        {
            std::vector<iq::ComplexF32> out(kChirpCount);
            for (size_t i = 0; i < kChirpCount; ++i)
                out[i] = {chirps[i].real(), chirps[i].imag()};
            return out;
        }
        case iq::ChirpMode::Average:
        {
            std::complex<float> sum{0.0f, 0.0f};
            for (const auto& sample : chirps)
                sum += sample;
            sum /= static_cast<float>(kChirpCount);
            return {{sum.real(), sum.imag()}};
        }
        case iq::ChirpMode::MaxAbs:
        {
            size_t best_idx = 0;
            float best_norm = complexNormSq(chirps[0]);
            for (size_t i = 1; i < kChirpCount; ++i)
            {
                const float n = complexNormSq(chirps[i]);
                if (n > best_norm)
                {
                    best_norm = n;
                    best_idx = i;
                }
            }
            return {{chirps[best_idx].real(), chirps[best_idx].imag()}};
        }
        }
        return {};
    }

    template <typename Cube, typename Req, typename Msg>
    std::array<std::complex<float>, kChirpCount> extractRangeSlowTime(
        const Cube& cube,
        const Req& req,
        const Msg& msg,
        const std::array<std::complex<float>, kVaCount>* steer,
        int range_bin)
    {
        std::array<std::complex<float>, kChirpCount> chirps{};
        const uint64_t chirp_mask = msg.chirp_mask.value_or(iq::kChirpMaskAll);
        for (size_t chirp = 0; chirp < kChirpCount; ++chirp)
        {
            if (!chirpEnabled(req.chirp_select_mode, chirp_mask, chirp))
                continue;

            switch (req.va_combine_mode)
            {
            case iq::VaCombineMode::MultiVa:
                chirps[chirp] = singleVaAt(cube, req.tile, range_bin, chirp, req.sub_ant);
                break;
            case iq::VaCombineMode::AverageAll:
                chirps[chirp] = averageAllAt(
                    cube, range_bin, chirp, msg, req.va_select_mode, req.tile, req.sub_ant);
                break;
            case iq::VaCombineMode::Beamform:
                chirps[chirp] = beamformAt(cube, *steer, range_bin, chirp);
                break;
            }
        }
        return chirps;
    }

    template <typename Cube, typename Req, typename Msg>
    std::vector<iq::ComplexF32> extractPayload(
        const Cube& cube,
        const Req& req,
        const Msg& msg,
        const std::array<std::complex<float>, kVaCount>* steer,
        int range_bin,
        iq::ChirpMode chirp_mode)
    {
        if (chirp_mode != iq::ChirpMode::Array)
        {
            const auto sample = extractCombinedSample(cube, req, msg, steer, range_bin, chirp_mode);
            return {{sample.real(), sample.imag()}};
        }

        const auto chirps = extractRangeSlowTime(cube, req, msg, steer, range_bin);
        return reduceChirps(chirps, chirp_mode);
    }

    const std::array<std::complex<float>, kVaCount>* steerFor(
        const SteeringCache& cache,
        iq::VaCombineMode va_mode,
        float azimuth_rad,
        float elevation_rad)
    {
        if (va_mode != iq::VaCombineMode::Beamform)
            return nullptr;
        return &cache.get(azimuth_rad, elevation_rad);
    }
}

iq::IqResponse SignalProcessor::processIq(const RangeCube& cube, const iq::IqRequestMsg& msg) const
{
    iq::IqResponse response{};
    response.header.status = static_cast<uint32_t>(iq::IqStatus::Ok);
    response.info.target_count = msg.request.target_count;

    if (msg.distances.size() != msg.request.target_count)
    {
        response.header.status = static_cast<uint32_t>(iq::IqStatus::InvalidRequest);
        response.info.target_count = 0;
        return response;
    }

    const iq::ChirpMode mode = msg.request.chirp_mode;
    response.payload.reserve(msg.request.target_count * chirpOutputCount(mode));

    const auto* steer = steerFor(m_steerCache, msg.request.va_combine_mode,
        msg.request.azimuth, msg.request.elevation);

    for (float distance_m : msg.distances)
    {
        const int range_bin = CubeMmap::rangeBinFromDistance(distance_m);
        const auto target_payload = extractPayload(
            cube, msg.request, msg, steer, range_bin, mode);
        response.payload.insert(response.payload.end(), target_payload.begin(), target_payload.end());
    }

    return response;
}

iq::RdmResponse SignalProcessor::processRdm(const DopplerCube& doppler_cube, const iq::RdmRequestMsg& msg) const
{
    iq::RdmResponse response{};
    response.header.status = static_cast<uint32_t>(iq::IqStatus::Ok);

    const auto& spec = msg.request;
    const int range_bin_min = CubeMmap::rangeBinFromDistance(spec.distance_min);
    const int range_bin_max = CubeMmap::rangeBinFromDistance(spec.distance_max);
    if (range_bin_min > range_bin_max)
    {
        response.header.status = static_cast<uint32_t>(iq::IqStatus::InvalidRequest);
        return response;
    }

    std::vector<int> doppler_bins;
    if (spec.chirp_mode == iq::ChirpMode::Array)
    {
        doppler_bins.reserve(kDopplerCount);
        for (int d = 0; d < static_cast<int>(kDopplerCount); ++d)
        {
            const float velocity = CubeMmap::velocityFromDopplerBin(d);
            if (velocity >= spec.velocity_min && velocity <= spec.velocity_max)
                doppler_bins.push_back(d);
        }
        std::sort(doppler_bins.begin(), doppler_bins.end(), [](int a, int b) {
            return CubeMmap::velocityFromDopplerBin(a) < CubeMmap::velocityFromDopplerBin(b);
        });
    }
    else if (0.0f >= spec.velocity_min && 0.0f <= spec.velocity_max)
    {
        doppler_bins.push_back(kZeroDopplerBin);
    }

    const size_t range_count = static_cast<size_t>(range_bin_max - range_bin_min + 1);
    const size_t doppler_count = doppler_bins.size();

    if (range_count == 0 || doppler_count == 0 || range_count * doppler_count > iq::kMaxRdmCells)
    {
        response.header.status = static_cast<uint32_t>(iq::IqStatus::InvalidRequest);
        return response;
    }

    response.info.range_count = static_cast<uint16_t>(range_count);
    response.info.doppler_count = static_cast<uint16_t>(doppler_count);
    response.info.range_min_m = static_cast<float>(range_bin_min) * kRangeResolutionM;
    response.info.range_step_m = kRangeResolutionM;
    response.info.velocity_min_mps = CubeMmap::velocityFromDopplerBin(doppler_bins.front());
    response.info.velocity_step_mps = (doppler_count > 1) ? kVelocityResolutionMps : 0.0f;

    response.payload.resize(range_count * doppler_count);

    const auto* steer = steerFor(m_steerCache, spec.va_combine_mode, spec.azimuth, spec.elevation);

    size_t out_idx = 0;
    switch (spec.va_combine_mode)
    {
    case iq::VaCombineMode::MultiVa:
        for (int range_bin = range_bin_min; range_bin <= range_bin_max; ++range_bin)
        {
            for (const int d : doppler_bins)
            {
                const auto value = singleVaAt(
                    doppler_cube,
                    spec.tile,
                    range_bin,
                    static_cast<size_t>(d),
                    spec.sub_ant);
                response.payload[out_idx++] = {value.real(), value.imag()};
            }
        }
        break;
    case iq::VaCombineMode::AverageAll:
        for (int range_bin = range_bin_min; range_bin <= range_bin_max; ++range_bin)
        {
            for (const int d : doppler_bins)
            {
                const auto value = averageAllAt(
                    doppler_cube,
                    range_bin,
                    static_cast<size_t>(d),
                    msg,
                    spec.va_select_mode,
                    spec.tile,
                    spec.sub_ant);
                response.payload[out_idx++] = {value.real(), value.imag()};
            }
        }
        break;
    case iq::VaCombineMode::Beamform:
        for (int range_bin = range_bin_min; range_bin <= range_bin_max; ++range_bin)
        {
            for (const int d : doppler_bins)
            {
                const auto value = beamformAt(doppler_cube, *steer, range_bin, static_cast<size_t>(d));
                response.payload[out_idx++] = {value.real(), value.imag()};
            }
        }
        break;
    }

    return response;
}
