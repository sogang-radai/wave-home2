#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "../core/coredefs.h"

WAVE_NAMESPACE_BEGIN

struct LaunchOptions
{
    std::string config_path = "config.json";
    std::string profile = "real";
    std::optional<uint16_t> port;
    std::optional<std::string> document_root;
    bool no_devices = false;
};

WAVE_NAMESPACE_END
