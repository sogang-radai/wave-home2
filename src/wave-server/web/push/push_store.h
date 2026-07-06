#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <drogon/orm/DbClient.h>

#include "../../core/coredefs.h"
#include "web_push.h"

WAVE_NAMESPACE_BEGIN
namespace web {
namespace push {

bool upsertSubscription(
    const drogon::orm::DbClientPtr& client,
    int64_t session_id,
    const Subscription& subscription);

bool deleteSubscriptions(const drogon::orm::DbClientPtr& client, int64_t session_id);

std::vector<Subscription> listSubscriptions(
    const drogon::orm::DbClientPtr& client,
    int64_t session_id);

} // namespace push
} // namespace web
WAVE_NAMESPACE_END
