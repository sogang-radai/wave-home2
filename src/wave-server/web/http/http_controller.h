#pragma once

#include <functional>

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include "../../core/coredefs.h"

WAVE_NAMESPACE_BEGIN
WEB_NAMESPACE_BEGIN

using HttpRequestPtr = drogon::HttpRequestPtr;
using HttpResponseCallback = std::function<void(const drogon::HttpResponsePtr&)>;

WEB_NAMESPACE_END
WAVE_NAMESPACE_END
