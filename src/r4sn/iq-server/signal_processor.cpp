#include "signal_processor.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <limits>
#include "calibration_coefs.h"
#include "radar_params.hpp"
#include "va_geometry.h"

namespace r4sn {
namespace {

constexpr float kTwoPiOverLambda = 6.28318530718f / embedded::kWavelength;

inline std::size_t coefIndex(const std::size_t tile, const std::size_t sub_ant)
{
    return tile * 4 + (sub_ant % 4) + 48 * (sub_ant / 4);
}

class SteeringTable
{
public:
    static SteeringTable fromAngles(const float azimuth_rad, const float elevation_rad)
    {
        SteeringTable table{};
        const float sin_theta = std::sin(azimuth_rad);
        const float cos_phi = std::cos(elevation_rad);
        const float sin_phi = std::sin(elevation_rad);

        for (std::size_t va_idx = 0; va_idx < kVaCount; ++va_idx) {
            const float x = embedded::kVaX[va_idx];
            const float y = embedded::kVaY[va_idx];
            const float phase = -kTwoPiOverLambda * (x * sin_theta * cos_phi + y * sin_phi);
            table.weights_[va_idx] = {std::cos(phase), std::sin(phase)};
        }
        return table;
    }

    const std::complex<float>& operator[](const std::size_t va_idx) const { return weights_[va_idx]; }

private:
    std::array<std::complex<float>, kVaCount> weights_{};
};

inline std::complex<float> readRawSample(
    const RangeCube& cube,
    const std::size_t tile,
    const int range_bin,
    const std::size_t slow_idx,
    const std::size_t sub_ant)
{
    return {static_cast<float>(cube[tile][range_bin][slow_idx][sub_ant][1]),
            static_cast<float>(cube[tile][range_bin][slow_idx][sub_ant][0])};
}

inline std::complex<float> beamformAt(
    const RangeCube& cube,
    const SteeringTable& steer,
    const int range_bin,
    const std::size_t slow_idx)
{
    std::complex<float> sum{0.0f, 0.0f};
    for (std::size_t tile = 0; tile < kTileCount; ++tile) {
        for (std::size_t sub_ant = 0; sub_ant < kSubAntCount; ++sub_ant) {
            const std::size_t va_idx = tile * kSubAntCount + sub_ant;
            const auto raw = readRawSample(cube, tile, range_bin, slow_idx, sub_ant);
            sum += raw * steer[va_idx];
        }
    }
    return sum;
}

std::array<std::complex<float>, kChirpCount> beamformSlowTime(
    const RangeCube& cube,
    const SteeringTable& steer,
    const int range_bin)
{
    std::array<std::complex<float>, kChirpCount> chirps{};
    for (std::size_t chirp = 0; chirp < kChirpCount; ++chirp) {
        chirps[chirp] = beamformAt(cube, steer, range_bin, chirp);
    }
    return chirps;
}

std::vector<iq::ComplexF32> reduceChirps(const std::array<std::complex<float>, kChirpCount>& chirps, const iq::ChirpMode mode)
{
    switch (mode) {
    case iq::ChirpMode::PerChirp: {
        std::vector<iq::ComplexF32> out(kChirpCount);
        for (std::size_t i = 0; i < kChirpCount; ++i) {
            out[i] = {chirps[i].real(), chirps[i].imag()};
        }
        return out;
    }
    case iq::ChirpMode::Average: {
        std::complex<float> sum{0.0f, 0.0f};
        for (const auto& sample : chirps) {
            sum += sample;
        }
        sum /= static_cast<float>(kChirpCount);
        return {{sum.real(), sum.imag()}};
    }
    case iq::ChirpMode::MaxAbs: {
        const auto best = std::max_element(
            chirps.begin(),
            chirps.end(),
            [](const std::complex<float>& a, const std::complex<float>& b) {
                return std::norm(a) < std::norm(b);
            });
        return {{best->real(), best->imag()}};
    }
    case iq::ChirpMode::FirstChirp:
        return {{chirps[0].real(), chirps[0].imag()}};
    }
    return {};
}

std::complex<float> reduceToSingle(const std::array<std::complex<float>, kChirpCount>& chirps, const iq::ChirpMode mode)
{
    switch (mode) {
    case iq::ChirpMode::Average: {
        std::complex<float> sum{0.0f, 0.0f};
        for (const auto& sample : chirps) {
            sum += sample;
        }
        return sum / static_cast<float>(kChirpCount);
    }
    case iq::ChirpMode::MaxAbs: {
        const auto best = std::max_element(
            chirps.begin(),
            chirps.end(),
            [](const std::complex<float>& a, const std::complex<float>& b) {
                return std::norm(a) < std::norm(b);
            });
        return *best;
    }
    case iq::ChirpMode::FirstChirp:
        return chirps[0];
    default:
        return {0.0f, 0.0f};
    }
}

std::array<std::complex<float>, kChirpCount> extractRangeSlowTime(
    const RangeCube& cube,
    const iq::VaCombineMode va_mode,
    const uint8_t tile,
    const uint8_t sub_ant,
    const SteeringTable& steer,
    const int range_bin)
{
    std::array<std::complex<float>, kChirpCount> chirps{};
    for (std::size_t chirp = 0; chirp < kChirpCount; ++chirp) {
        if (va_mode == iq::VaCombineMode::SingleVa) {
            chirps[chirp] = readRawSample(cube, tile, range_bin, chirp, sub_ant);
        } else if (va_mode == iq::VaCombineMode::AverageAll) {
            std::complex<float> sum{0.0f, 0.0f};
            for (std::size_t t = 0; t < kTileCount; ++t) {
                for (std::size_t sa = 0; sa < kSubAntCount; ++sa) {
                    sum += readRawSample(cube, t, range_bin, chirp, sa);
                }
            }
            chirps[chirp] = sum / static_cast<float>(kVaCount);
        } else {
            chirps[chirp] = beamformAt(cube, steer, range_bin, chirp);
        }
    }
    return chirps;
}

}  // namespace

std::vector<iq::ComplexF32> SignalProcessor::processTarget(const RangeCube& cube, const iq::TargetSpec& target) const
{
    const int range_bin = CubeMmap::rangeBinFromDistance(target.distance_m);
    const auto steer = SteeringTable::fromAngles(target.azimuth_rad, target.elevation_rad);
    const auto chirps = extractRangeSlowTime(cube, iq::VaCombineMode::Beamform, 0, 0, steer, range_bin);
    return reduceChirps(chirps, target.chirp_mode);
}

std::vector<iq::ComplexF32> SignalProcessor::processRequest(
    const RangeCube& cube,
    const std::vector<iq::TargetSpec>& targets,
    const iq::VaCombineMode va_mode,
    const uint8_t tile,
    const uint8_t sub_ant) const
{
    std::vector<iq::ComplexF32> payload;
    if (targets.empty()) {
        return payload;
    }

    payload.reserve(targets.size() * kChirpCount);

    const auto steer = SteeringTable::fromAngles(targets.front().azimuth_rad, targets.front().elevation_rad);
    const iq::ChirpMode mode = targets.front().chirp_mode;

    const bool uniform = std::all_of(
        targets.begin(),
        targets.end(),
        [&](const iq::TargetSpec& t) {
            return t.azimuth_rad == targets.front().azimuth_rad
                   && t.elevation_rad == targets.front().elevation_rad
                   && t.chirp_mode == mode;
        });

    if (uniform) {
        for (const iq::TargetSpec& target : targets) {
            const int range_bin = CubeMmap::rangeBinFromDistance(target.distance_m);
            const auto chirps = extractRangeSlowTime(cube, va_mode, tile, sub_ant, steer, range_bin);
            const auto target_payload = reduceChirps(chirps, mode);
            payload.insert(payload.end(), target_payload.begin(), target_payload.end());
        }
        return payload;
    }

    float last_az = std::numeric_limits<float>::quiet_NaN();
    float last_el = std::numeric_limits<float>::quiet_NaN();
    SteeringTable cached_steer = steer;

    for (const iq::TargetSpec& target : targets) {
        if (va_mode == iq::VaCombineMode::Beamform
            && (target.azimuth_rad != last_az || target.elevation_rad != last_el)) {
            cached_steer = SteeringTable::fromAngles(target.azimuth_rad, target.elevation_rad);
            last_az = target.azimuth_rad;
            last_el = target.elevation_rad;
        }

        const int range_bin = CubeMmap::rangeBinFromDistance(target.distance_m);
        const auto chirps = extractRangeSlowTime(cube, va_mode, tile, sub_ant, cached_steer, range_bin);
        const auto target_payload = reduceChirps(chirps, target.chirp_mode);
        payload.insert(payload.end(), target_payload.begin(), target_payload.end());
    }

    return payload;
}

iq::RdmResponse SignalProcessor::processRdm(const DopplerCube& doppler_cube, const iq::RdmRequestSpec& spec) const
{
    iq::RdmResponse response{};
    response.header.magic = iq::kResponseMagic;
    response.header.version = iq::kProtocolVersion2;
    response.header.request_type = static_cast<uint16_t>(iq::RequestType::RangeDopplerMap);
    response.header.status = static_cast<uint32_t>(iq::IqStatus::Ok);

    const int range_bin_min = CubeMmap::rangeBinFromDistance(spec.distance_min_m);
    const int range_bin_max = CubeMmap::rangeBinFromDistance(spec.distance_max_m);
    if (range_bin_min > range_bin_max) {
        response.header.status = static_cast<uint32_t>(iq::IqStatus::InvalidRequest);
        return response;
    }

    std::vector<int> doppler_bins;
    if (spec.chirp_mode == iq::ChirpMode::PerChirp) {
        for (int d = 0; d < static_cast<int>(kDopplerCount); ++d) {
            const float velocity = CubeMmap::velocityFromDopplerBin(d);
            if (velocity >= spec.velocity_min_mps && velocity <= spec.velocity_max_mps) {
                doppler_bins.push_back(d);
            }
        }
        std::sort(doppler_bins.begin(), doppler_bins.end(), [](const int a, const int b) {
            return CubeMmap::velocityFromDopplerBin(a) < CubeMmap::velocityFromDopplerBin(b);
        });
    } else if (0.0f >= spec.velocity_min_mps && 0.0f <= spec.velocity_max_mps) {
        doppler_bins.push_back(kZeroDopplerBin);
    }

    const std::size_t range_count = static_cast<std::size_t>(range_bin_max - range_bin_min + 1);
    const std::size_t doppler_count = doppler_bins.size();

    if (range_count == 0 || doppler_count == 0 || range_count * doppler_count > iq::kMaxRdmCells) {
        response.header.status = static_cast<uint32_t>(iq::IqStatus::InvalidRequest);
        return response;
    }

    response.header.range_count = static_cast<uint16_t>(range_count);
    response.header.doppler_count = static_cast<uint16_t>(doppler_count);
    response.header.range_min_m = static_cast<float>(range_bin_min) * firmware::kRangeResolutionM;
    response.header.range_step_m = firmware::kRangeResolutionM;
    response.header.velocity_min_mps = CubeMmap::velocityFromDopplerBin(doppler_bins.front());
    response.header.velocity_step_mps = (doppler_count > 1) ? firmware::kVelocityResolutionMps : 0.0f;

    response.payload.reserve(range_count * doppler_count);

    const bool use_beamform = spec.va_combine_mode == iq::VaCombineMode::Beamform;
    const SteeringTable steer = use_beamform
        ? SteeringTable::fromAngles(spec.azimuth_rad, spec.elevation_rad)
        : SteeringTable{};

    auto extract_cell = [&](const int range_bin, const std::size_t doppler_bin) -> std::complex<float> {
        if (spec.va_combine_mode == iq::VaCombineMode::SingleVa) {
            return readRawSample(
                doppler_cube,
                spec.tile,
                range_bin,
                doppler_bin,
                spec.sub_ant);
        }

        if (spec.va_combine_mode == iq::VaCombineMode::AverageAll) {
            std::complex<float> sum{0.0f, 0.0f};
            for (std::size_t tile = 0; tile < kTileCount; ++tile) {
                for (std::size_t sub_ant = 0; sub_ant < kSubAntCount; ++sub_ant) {
                    sum += readRawSample(doppler_cube, tile, range_bin, doppler_bin, sub_ant);
                }
            }
            return sum / static_cast<float>(kVaCount);
        }

        return beamformAt(doppler_cube, steer, range_bin, doppler_bin);
    };

    for (int range_bin = range_bin_min; range_bin <= range_bin_max; ++range_bin) {
        for (const int d : doppler_bins) {
            const auto value = extract_cell(range_bin, static_cast<std::size_t>(d));
            response.payload.push_back({value.real(), value.imag()});
        }
    }

    return response;
}

}  // namespace r4sn
