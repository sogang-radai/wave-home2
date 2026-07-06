#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "../../core/coredefs.h"

WAVE_NAMESPACE_BEGIN
namespace web {
namespace push {

struct VapidConfig
{
    std::string public_key;
    std::string private_key;
    std::string subject = "mailto:wavehome@local";
};

struct Subscription
{
    std::string endpoint;
    std::string p256dh;
    std::string auth;
};

struct Message
{
    std::string title;
    std::string body;
    std::string url;
};

struct PreparedRequest
{
    std::string endpoint;
    std::unordered_map<std::string, std::string> headers;
    std::vector<uint8_t> body;
};

std::optional<PreparedRequest> prepareRequest(
    const Subscription& subscription,
    const VapidConfig& vapid,
    const Message& message);

} // namespace push
} // namespace web
WAVE_NAMESPACE_END
