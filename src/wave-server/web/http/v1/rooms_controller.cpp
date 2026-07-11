#include "rooms_controller.h"

#include "../../../app/app_state.h"
#include "../../../device/device_wire_id.hpp"
#include "rooms_store.h"
#include "session_store.h"

WAVE_NAMESPACE_BEGIN
WEB_NAMESPACE_BEGIN
namespace v1 {

void RoomsController::listRooms(
    const drogon::HttpRequestPtr& /*req*/,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    auto& state = AppState::get();
    if (!state.db())
    {
        respondError(callback, 503, "DB_UNAVAILABLE", "데이터베이스를 사용할 수 없습니다.");
        return;
    }

    RoomsStore store(state.db());
    callback(drogon::HttpResponse::newHttpJsonResponse(store.listRooms()));
}

void RoomsController::createRoom(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    auto& state = AppState::get();
    if (!state.db())
    {
        respondError(callback, 503, "DB_UNAVAILABLE", "데이터베이스를 사용할 수 없습니다.");
        return;
    }

    const auto json = req->getJsonObject();
    if (!json || !json->isMember("name") || !(*json)["name"].isString())
    {
        respondError(callback, 400, "INVALID_NAME", "방 이름을 입력해주세요.", "name");
        return;
    }

    const std::string description = json->isMember("description") && (*json)["description"].isString()
        ? (*json)["description"].asString()
        : "";

    RoomsStore store(state.db());
    std::string error;
    std::string field;
    const auto room = store.createRoom((*json)["name"].asString(), description, error, field);
    if (!room)
    {
        respondError(callback, 400, "INVALID_NAME", error, field);
        return;
    }

    Json::Value body;
    body["id"] = dev::wireIdForDbRow(room->id);
    body["name"] = room->name;
    body["description"] = room->description;
    auto resp = drogon::HttpResponse::newHttpJsonResponse(body);
    resp->setStatusCode(drogon::k201Created);
    callback(resp);
}

void RoomsController::updateRoom(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    int64_t roomId)
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

    RoomsStore store(state.db());
    std::string error;
    std::string field;
    const auto room = store.updateRoom(roomId, *json, error, field);
    if (!room)
    {
        const auto status = field.empty() ? 404 : 400;
        const auto code = field.empty() ? "NOT_FOUND" : "INVALID_NAME";
        respondError(callback, status, code, error, field);
        return;
    }

    Json::Value body;
    body["id"] = dev::wireIdForDbRow(room->id);
    body["name"] = room->name;
    body["description"] = room->description;
    callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

void RoomsController::deleteRoom(
    const drogon::HttpRequestPtr& /*req*/,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    int64_t roomId)
{
    auto& state = AppState::get();
    if (!state.db())
    {
        respondError(callback, 503, "DB_UNAVAILABLE", "데이터베이스를 사용할 수 없습니다.");
        return;
    }

    RoomsStore store(state.db());
    std::string error;
    std::string code;
    if (!store.deleteRoom(roomId, error, code))
    {
        const auto status = code == "ROOM_HAS_DEVICES" ? 409 : 404;
        respondError(callback, status, code, error);
        return;
    }

    Json::Value body;
    body["id"] = dev::wireIdForDbRow(roomId);
    callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

void RoomsController::getMembers(
    const drogon::HttpRequestPtr& /*req*/,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    int64_t roomId)
{
    auto& state = AppState::get();
    if (!state.db())
    {
        respondError(callback, 503, "DB_UNAVAILABLE", "데이터베이스를 사용할 수 없습니다.");
        return;
    }

    RoomsStore store(state.db());
    if (!store.findRoom(roomId))
    {
        respondError(callback, 404, "NOT_FOUND", "방을 찾을 수 없습니다.");
        return;
    }

    callback(drogon::HttpResponse::newHttpJsonResponse(store.listMembers(roomId)));
}

void RoomsController::putMembers(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    int64_t roomId)
{
    auto& state = AppState::get();
    if (!state.db())
    {
        respondError(callback, 503, "DB_UNAVAILABLE", "데이터베이스를 사용할 수 없습니다.");
        return;
    }

    const auto json = req->getJsonObject();
    if (!json || !json->isMember("accountIds") || !(*json)["accountIds"].isArray())
    {
        respondError(callback, 400, "INVALID_BODY", "accountIds 배열이 필요합니다.");
        return;
    }

    std::vector<int64_t> account_ids;
    for (const auto& item : (*json)["accountIds"])
        account_ids.push_back(item.asInt64());

    RoomsStore store(state.db());
    std::string error;
    std::string code;
    const auto members = store.updateMembers(roomId, account_ids, error, code);
    if (!members)
    {
        const auto status = code == "NOT_FOUND" ? 404 : 400;
        respondError(callback, status, code, error);
        return;
    }

    callback(drogon::HttpResponse::newHttpJsonResponse(*members));
}

} // namespace v1
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
