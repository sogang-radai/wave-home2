#pragma once

#include <string>

#include <json/json.h>

#include "core/coredefs.h"

WAVE_NAMESPACE_BEGIN

Json::Value demoSeedStateForClass(const std::string& device_class);
Json::Value demoApplyAction(
    const std::string& device_class,
    const Json::Value& prev_state,
    const std::string& action_name,
    const Json::Value& params);

WAVE_NAMESPACE_END
