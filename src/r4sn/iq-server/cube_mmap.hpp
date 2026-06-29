#pragma once

#include <cstdint>
#include <cstddef>
#include <stdexcept>
#include <string>

#include "radar_params.hpp"

constexpr uintptr_t kRangeCubePhysAddr = 0x8BE00000u;
constexpr uintptr_t kDopplerCubePhysAddr = 0x8CA00000u;
constexpr size_t kCubeSizeBytes = 12u * 1024u * 1024u;
constexpr size_t kTileCount = 12;
constexpr size_t kRangeCount = 256;
constexpr size_t kChirpCount = 64;
constexpr size_t kDopplerCount = 64;
constexpr size_t kSubAntCount = 16;
constexpr size_t kVaCount = kTileCount * kSubAntCount;
constexpr int kZeroDopplerBin = 0;
constexpr float kRangeBinSizeM = kRangeResolutionM;

using RangeCube = int16_t[kTileCount][kRangeCount][kChirpCount][kSubAntCount][2];
using DopplerCube = int16_t[kTileCount][kRangeCount][kDopplerCount][kSubAntCount][2];

class CubeMmap
{
public:
    CubeMmap();
    ~CubeMmap();

    CubeMmap(const CubeMmap&) = delete;
    CubeMmap& operator=(const CubeMmap&) = delete;

    const RangeCube& rangeCube() const { return *m_range_cube; }
    const DopplerCube& dopplerCube() const { return *m_doppler_cube; }

    const RangeCube& cube() const { return rangeCube(); }

    static int rangeBinFromDistance(float distance_m);
    static int dopplerBinFromVelocity(float velocity_mps);
    static float velocityFromDopplerBin(int doppler_bin);

private:
    void* mapAt(std::uintptr_t phys_addr);

    int m_fd = -1;
    void* m_range_mapped = nullptr;
    void* m_doppler_mapped = nullptr;
    const RangeCube* m_range_cube = nullptr;
    const DopplerCube* m_doppler_cube = nullptr;
};
