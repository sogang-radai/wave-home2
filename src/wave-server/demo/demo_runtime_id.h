#pragma once

#include <optional>
#include <string>

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include "core/coredefs.h"

WAVE_NAMESPACE_BEGIN

constexpr const char* kDemoRuntimeCookieName = "wavehome_demo_rid";
constexpr const char* kDemoRuntimeHeaderName = "X-Wave-Demo-Runtime-Id";

std::string generateDemoRuntimeId();
/** Remember the browser/chat session so agent calls without an id reuse it. */
void rememberPreferredDemoRuntimeId(const std::string& runtime_id);
std::optional<std::string> preferredDemoRuntimeId();
/** Prefer sticky session; only mint when none exists yet. */
std::string fallbackDemoRuntimeId();
std::optional<std::string> demoRuntimeIdFromCookie(const drogon::HttpRequestPtr& req);
std::optional<std::string> demoRuntimeIdFromHeader(const drogon::HttpRequestPtr& req);
void attachDemoRuntimeCookieIfNeeded(
    const drogon::HttpRequestPtr& req,
    const drogon::HttpResponsePtr& resp,
    const std::string& runtime_id);

WAVE_NAMESPACE_END
