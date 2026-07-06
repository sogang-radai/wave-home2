#pragma once

#include <chrono>
#include <string>

#include "coredefs.h"

WAVE_NAMESPACE_BEGIN

std::string formatTimestamp(std::chrono::system_clock::time_point tp = std::chrono::system_clock::now());

WAVE_NAMESPACE_END
