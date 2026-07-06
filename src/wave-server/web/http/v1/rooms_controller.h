#pragma once

#include <drogon/HttpController.h>

#include "core/coredefs.h"

WAVE_NAMESPACE_BEGIN
WEB_NAMESPACE_BEGIN
namespace v1 {

class RoomsController :
    public drogon::HttpController<RoomsController>
{
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(RoomsController::listRooms, "/api/v1/rooms", drogon::Get);
    ADD_METHOD_TO(RoomsController::createRoom, "/api/v1/rooms", drogon::Post);
    ADD_METHOD_TO(RoomsController::updateRoom, "/api/v1/rooms/{roomId}", drogon::Patch);
    ADD_METHOD_TO(RoomsController::deleteRoom, "/api/v1/rooms/{roomId}", drogon::Delete);
    ADD_METHOD_TO(RoomsController::getMembers, "/api/v1/rooms/{roomId}/members", drogon::Get);
    ADD_METHOD_TO(RoomsController::putMembers, "/api/v1/rooms/{roomId}/members", drogon::Put);
    METHOD_LIST_END

    void listRooms(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void createRoom(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void updateRoom(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback,
        int64_t roomId);

    void deleteRoom(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback,
        int64_t roomId);

    void getMembers(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback,
        int64_t roomId);

    void putMembers(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback,
        int64_t roomId);
};

} // namespace v1
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
