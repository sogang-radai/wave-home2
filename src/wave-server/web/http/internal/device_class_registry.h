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
    static Json::Value list_classes();
    static Json::Value capabilities_for_class(const std::string& class_name);
    static std::string label_for_class(const std::string& class_name);
};

} // namespace internal
} // namespace web
WAVE_NAMESPACE_END
