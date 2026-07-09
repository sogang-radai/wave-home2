#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../core/coredefs.h"

WAVE_NAMESPACE_BEGIN
SERVICE_NAMESPACE_BEGIN

std::string matchIrCommandId(const std::vector<uint16_t>& received_timings);
void notifyIrReceived(const std::string& device_id, const std::vector<uint16_t>& received_timings);

SERVICE_NAMESPACE_END
WAVE_NAMESPACE_END
