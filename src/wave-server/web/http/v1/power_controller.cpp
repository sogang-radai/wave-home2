#include "power_controller.h"

#include "../../../app/app_state.h"
#include "iot_store.h"
#include "power_store.h"
#include "session_store.h"

WAVE_NAMESPACE_BEGIN
WEB_NAMESPACE_BEGIN
namespace v1 {

void PowerController::listPlugs(
    const drogon::HttpRequestPtr& /*req*/,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    auto& state = AppState::get();
    IotStore iot(state.deviceManager);
    if (!iot.devicesAvailable())
    {
        respondError(callback, 503, "DEVICES_UNAVAILABLE", "장치 관리자를 사용할 수 없습니다.");
        return;
    }

    PowerStore store(iot);
    callback(drogon::HttpResponse::newHttpJsonResponse(store.listPlugs()));
}

void PowerController::comboTrend(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    auto& state = AppState::get();
    IotStore iot(state.deviceManager);
    if (!iot.devicesAvailable())
    {
        respondError(callback, 503, "DEVICES_UNAVAILABLE", "장치 관리자를 사용할 수 없습니다.");
        return;
    }

    const auto device_id = req->getParameter("deviceId");
    const auto range = req->getParameter("range");
    const auto metric = req->getParameter("metric");
    if (device_id.empty() || range.empty())
    {
        respondError(callback, 400, "INVALID_QUERY", "deviceId와 range가 필요합니다.");
        return;
    }

    PowerStore store(iot);
    callback(drogon::HttpResponse::newHttpJsonResponse(
        store.comboTrend(device_id, range, metric.empty() ? "w" : metric)));
}

} // namespace v1
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
