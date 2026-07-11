#include "power_controller.h"

#include "../../../app/app_state.h"
#include "../../../demo/demo_device_backend.h"
#include "../../../demo/demo_power_meter.h"
#include "../../../demo/demo_runtime_id.h"
#include "iot_store.h"
#include "power_store.h"
#include "session_store.h"

WAVE_NAMESPACE_BEGIN
WEB_NAMESPACE_BEGIN
namespace v1 {

void PowerController::listPlugs(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    if (demoVirtualDevicesEnabled())
    {
        const auto runtime_id = resolveDemoRuntimeId(req, nullptr);
        auto resp = drogon::HttpResponse::newHttpJsonResponse(
            DemoPowerMeter::instance().listPlugs(runtime_id, AppState::get().db()));
        attachDemoRuntimeCookieIfNeeded(req, resp, runtime_id);
        callback(resp);
        return;
    }

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
    const auto device_id = req->getParameter("deviceId");
    const auto range = req->getParameter("range");
    const auto metric = req->getParameter("metric");
    if (device_id.empty() || range.empty())
    {
        respondError(callback, 400, "INVALID_QUERY", "deviceId와 range가 필요합니다.");
        return;
    }

    if (demoVirtualDevicesEnabled())
    {
        const auto runtime_id = resolveDemoRuntimeId(req, nullptr);
        auto resp = drogon::HttpResponse::newHttpJsonResponse(
            DemoPowerMeter::instance().comboTrend(runtime_id, device_id, range));
        attachDemoRuntimeCookieIfNeeded(req, resp, runtime_id);
        callback(resp);
        return;
    }

    auto& state = AppState::get();
    IotStore iot(state.deviceManager);
    if (!iot.devicesAvailable())
    {
        respondError(callback, 503, "DEVICES_UNAVAILABLE", "장치 관리자를 사용할 수 없습니다.");
        return;
    }

    PowerStore store(iot);
    callback(drogon::HttpResponse::newHttpJsonResponse(
        store.comboTrend(device_id, range, metric.empty() ? "w" : metric)));
}

void PowerController::periodTrend(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    auto client = AppState::get().db();
    if (!client)
    {
        respondError(callback, 503, "DB_UNAVAILABLE", "데이터베이스를 사용할 수 없습니다.");
        return;
    }

    const auto device_id = req->getParameter("deviceId");
    const auto period = req->getParameter("period");
    const auto ref_date = req->getParameter("refDate");
    if (device_id.empty() || period.empty())
    {
        respondError(callback, 400, "INVALID_QUERY", "deviceId와 period가 필요합니다.");
        return;
    }

    callback(drogon::HttpResponse::newHttpJsonResponse(
        PowerStore::periodTrend(client, device_id, period, ref_date)));
}

void PowerController::powerReport(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    auto client = AppState::get().db();
    if (!client)
    {
        respondError(callback, 503, "DB_UNAVAILABLE", "데이터베이스를 사용할 수 없습니다.");
        return;
    }

    const auto device_id = req->getParameter("deviceId");
    const auto period = req->getParameter("period");
    const auto period_start = req->getParameter("periodStart");
    if (device_id.empty() || period.empty())
    {
        respondError(callback, 400, "INVALID_QUERY", "deviceId와 period가 필요합니다.");
        return;
    }

    callback(drogon::HttpResponse::newHttpJsonResponse(
        PowerStore::queryReport(client, device_id, period, period_start)));
}

} // namespace v1
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
