#pragma once

#include <string>

#include <json/json.h>

#include "core/coredefs.h"

WAVE_NAMESPACE_BEGIN
namespace web {
namespace internal {

class DeviceClassRegistry
{
public:
    static Json::Value listClasses();
    static Json::Value capabilitiesForClass(const std::string& class_name);
    static std::string labelForClass(const std::string& class_name);
};

} // namespace internal
} // namespace web
WAVE_NAMESPACE_END
