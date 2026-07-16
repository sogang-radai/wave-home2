#include "devices_controller.h"

#include "../../../app/app_state.h"
#include "../../../device/device.h"
#include "devices_store.h"
#include "session_store.h"

WAVE_NAMESPACE_BEGIN
WEB_NAMESPACE_BEGIN
namespace v1 {

void DevicesController::listDevices(const HttpRequestPtr& /*req*/, HttpResponseCallback&& callback)
{
    auto& state = AppState::get();
    if (!state.db())
    {
        respondError(callback, 503, "DB_UNAVAILABLE", "데이터베이스를 사용할 수 없습니다.");
        return;
    }

    DevicesStore store(state.db());
    callback(drogon::HttpResponse::newHttpJsonResponse(store.listDevices()));
}

void DevicesController::createDevice(const HttpRequestPtr& req, HttpResponseCallback&& callback)
{
    auto& state = AppState::get();
    if (!state.db())
    {
        respondError(callback, 503, "DB_UNAVAILABLE", "데이터베이스를 사용할 수 없습니다.");
        return;
    }

    const auto json = req->getJsonObject();
    if (!json || !json->isObject())
    {
        respondError(callback, 400, "INVALID_BODY", "JSON 본문이 필요합니다.");
        return;
    }

    DevicesStore store(state.db());
    std::string error;
    std::string field;
    std::string code;
    const auto device = store.createDevice(*json, error, field, code);
    if (!device)
    {
        respondError(callback, 400, code, error, field);
        return;
    }

    auto resp = drogon::HttpResponse::newHttpJsonResponse(*device);
    resp->setStatusCode(drogon::k201Created);
    callback(resp);
}

void DevicesController::updateDevice(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string deviceId)
{
    auto& state = AppState::get();
    if (!state.db())
    {
        respondError(callback, 503, "DB_UNAVAILABLE", "데이터베이스를 사용할 수 없습니다.");
        return;
    }

    const auto json = req->getJsonObject();
    if (!json || !json->isObject())
    {
        respondError(callback, 400, "INVALID_BODY", "JSON 본문이 필요합니다.");
        return;
    }

    DevicesStore store(state.db());
    std::string error;
    std::string code;
    const auto device = store.updateDevice(deviceId, *json, error, code);
    if (!device)
    {
        const auto status = code == "NOT_FOUND" ? 404 : 400;
        respondError(callback, status, code, error);
        return;
    }

    callback(drogon::HttpResponse::newHttpJsonResponse(*device));
}

void DevicesController::deleteDevice(const HttpRequestPtr& /*req*/, HttpResponseCallback&& callback,
    std::string deviceId)
{
    auto& state = AppState::get();
    if (!state.db())
    {
        respondError(callback, 503, "DB_UNAVAILABLE", "데이터베이스를 사용할 수 없습니다.");
        return;
    }

    DevicesStore store(state.db());
    std::string error;
    if (!store.deleteDevice(deviceId, error))
    {
        respondError(callback, 404, "NOT_FOUND", error);
        return;
    }

    Json::Value body;
    body["id"] = deviceId;
    callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

void DevicesController::assignRoom(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string deviceId)
{
    auto& state = AppState::get();
    if (!state.db())
    {
        respondError(callback, 503, "DB_UNAVAILABLE", "데이터베이스를 사용할 수 없습니다.");
        return;
    }

    const auto json = req->getJsonObject();
    if (!json || !json->isMember("roomId"))
    {
        respondError(callback, 400, "INVALID_BODY", "roomId가 필요합니다.");
        return;
    }

    int64_t room_id = 0;
    if ((*json)["roomId"].isString())
        room_id = static_cast<int64_t>(dev::parseRoomID((*json)["roomId"].asString()));
    else
        room_id = (*json)["roomId"].asInt64();
    if (room_id <= 0)
    {
        respondError(callback, 400, "INVALID_BODY", "roomId가 유효하지 않습니다.");
        return;
    }

    DevicesStore store(state.db());
    std::string error;
    std::string code;
    const auto device = store.assignRoom(deviceId, room_id, error, code);
    if (!device)
    {
        const auto status = code == "NOT_FOUND" ? 404 : 400;
        respondError(callback, status, code, error);
        return;
    }

    callback(drogon::HttpResponse::newHttpJsonResponse(*device));
}

void DevicesController::unassignRoom(const HttpRequestPtr& /*req*/, HttpResponseCallback&& callback,
    std::string deviceId)
{
    auto& state = AppState::get();
    if (!state.db())
    {
        respondError(callback, 503, "DB_UNAVAILABLE", "데이터베이스를 사용할 수 없습니다.");
        return;
    }

    DevicesStore store(state.db());
    std::string error;
    const auto device = store.unassignRoom(deviceId, error);
    if (!device)
    {
        respondError(callback, 404, "NOT_FOUND", error);
        return;
    }

    callback(drogon::HttpResponse::newHttpJsonResponse(*device));
}

} // namespace v1
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
