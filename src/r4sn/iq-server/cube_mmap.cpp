#include "cube_mmap.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace r4sn
{
    CubeMmap::CubeMmap()
    {
        m_fd = open("/dev/mem", O_RDONLY | O_SYNC);
        if (m_fd < 0)
            throw std::runtime_error("failed to open /dev/mem");

        m_range_mapped = mapAt(kRangeCubePhysAddr);
        m_doppler_mapped = mapAt(kDopplerCubePhysAddr);
        m_range_cube = static_cast<const RangeCube*>(m_range_mapped);
        m_doppler_cube = static_cast<const DopplerCube*>(m_doppler_mapped);
    }

    CubeMmap::~CubeMmap()
    {
        if (m_range_mapped != MAP_FAILED)
            munmap(m_range_mapped, kCubeSizeBytes);
        if (m_doppler_mapped != MAP_FAILED)
            munmap(m_doppler_mapped, kCubeSizeBytes);
        if (m_fd >= 0)
            close(m_fd);
    }

    void* CubeMmap::mapAt(const std::uintptr_t phys_addr)
    {
        void* mapped = mmap(nullptr, kCubeSizeBytes, PROT_READ, MAP_SHARED, m_fd, static_cast<off_t>(phys_addr));
        if (mapped == MAP_FAILED)
            throw std::runtime_error("failed to mmap radar cube");
        return mapped;
    }

    int CubeMmap::rangeBinFromDistance(const float distance_m)
    {
        const int r_idx = static_cast<int>(std::lround(distance_m / kRangeBinSizeM));
        return std::clamp(r_idx, 0, static_cast<int>(kRangeCount) - 1);
    }

    int CubeMmap::dopplerBinFromVelocity(const float velocity_mps)
    {
        int k = static_cast<int>(std::lround(velocity_mps / firmware::kVelocityResolutionMps));
        k %= static_cast<int>(kDopplerCount);
        if (k < 0)
            k += static_cast<int>(kDopplerCount);
        return std::clamp(k, 0, static_cast<int>(kDopplerCount) - 1);
    }

    float CubeMmap::velocityFromDopplerBin(const int doppler_bin)
    {
        // Unshifted Doppler FFT layout: bin 0 = DC (v=0), bins 1..N/2-1 positive, N/2..N-1 negative.
        int k = doppler_bin;
        if (k >= static_cast<int>(kDopplerCount / 2))
            k -= static_cast<int>(kDopplerCount);
        return static_cast<float>(k) * firmware::kVelocityResolutionMps;
    }
}  // namespace r4sn
