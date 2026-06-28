#pragma once

#include <cstdint>
#include <cstddef>
#include <stdexcept>
#include <string>

#include "radar_params.hpp"

namespace r4sn
{
    // SPT pipeline (r4fn.elf.c / r4fn_SPTInitKernelData):
    //   rangeInputBufH    @ 0x8B200000  (12 MiB)
    //   rangeOutputBufH   @ 0x8BE00000  (12 MiB)  slow-time: 64 chirps
    //   dopplerOutputBufH @ 0x8CA00000  (12 MiB)  64 Doppler bins
    constexpr std::uintptr_t kRangeCubePhysAddr = 0x8BE00000u;
    constexpr std::uintptr_t kDopplerCubePhysAddr = 0x8CA00000u;
    constexpr std::size_t kCubeSizeBytes = 12u * 1024u * 1024u;
    constexpr std::size_t kTileCount = 12;
    constexpr std::size_t kRangeCount = 256;
    constexpr std::size_t kChirpCount = 64;
    constexpr std::size_t kDopplerCount = 64;
    constexpr std::size_t kSubAntCount = 16;
    constexpr std::size_t kVaCount = kTileCount * kSubAntCount;
    constexpr int kZeroDopplerBin = 0;  // unshifted Doppler FFT: DC at bin 0
    constexpr float kRangeBinSizeM = firmware::kRangeResolutionM;

    // Same memory layout; 3rd dimension is chirp (range cube) or Doppler bin (doppler cube).
    using RangeCube = int16_t[kTileCount][kRangeCount][kChirpCount][kSubAntCount][2];
    using DopplerCube = RangeCube;

    class CubeMmap
    {
    public:
        CubeMmap();
        ~CubeMmap();

        CubeMmap(const CubeMmap&) = delete;
        CubeMmap& operator=(const CubeMmap&) = delete;

        const RangeCube& rangeCube() const { return *m_range_cube; }
        const DopplerCube& dopplerCube() const { return *m_doppler_cube; }

        // Back-compat alias
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

}  // namespace r4sn
