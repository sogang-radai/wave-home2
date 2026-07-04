#pragma once

#include <cstdint>
#include <future>
#include <string>
#include <string_view>
#include <vector>

#include "../../core/coredefs.h"

WAVE_NAMESPACE_BEGIN
DEVICE_NAMESPACE_BEGIN

// As velocity: each axis in [-1.0, 1.0]. As position: each axis in [0.0, 1.0].
// Sign: pan +right/-left, tilt +up/-down, zoom +in/-out.
struct PtzVector
{
    float pan = 0.0f;
    float tilt = 0.0f;
    float zoom = 0.0f;
};

struct PtzPreset
{
    uint32_t id = 0;
    std::string name;
};

struct PtzCapabilities
{
    bool pan = false;
    bool tilt = false;
    bool zoom = false;
    bool absolute = false;
    bool presets = false;
    bool home = false;
    uint32_t maxPresets = 0;
};

class IPtzController
{
public:
    virtual ~IPtzController() = default;

    virtual PtzCapabilities getPtzCapabilities() const = 0;

    // durationMs == 0 keeps moving until stopPtz(); otherwise auto-stops after the given time.
    virtual bool movePtz(const PtzVector& velocity, uint32_t durationMs = 0) = 0;
    virtual bool stopPtz() = 0;

    // Requires PtzCapabilities::absolute.
    virtual bool movePtzTo(const PtzVector& position) = 0;

    virtual bool enumeratePtzPresets(std::vector<PtzPreset>& outPresets) = 0;
    virtual bool gotoPtzPreset(uint32_t presetId) = 0;
    virtual bool savePtzPreset(uint32_t presetId, std::string_view name) = 0;

    virtual bool movePtzHome() = 0;

    virtual std::future<bool> movePtzAsync(const PtzVector& velocity, uint32_t durationMs = 0) = 0;
    virtual std::future<bool> gotoPtzPresetAsync(uint32_t presetId) = 0;
};

DEVICE_NAMESPACE_END
WAVE_NAMESPACE_END
