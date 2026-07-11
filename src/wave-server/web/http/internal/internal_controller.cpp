#include "internal_controller.h"

#include <cstdint>
#include <optional>
#include <sstream>
#include <thread>
#include <vector>

#include "../../../app/app_state.h"
#include "../../../demo/demo_device_backend.h"
#include "../../../demo/demo_session_writes.h"
#include "../../../demo/demo_runtime_id.h"
#include "../../../service/alarm_manager.h"
#include "../v1/iot_store.h"
#include "../v1/session_store.h"
#include "alarms_internal_store.h"
#include "db_query_store.h"
#include "device_class_registry.h"
#include "devices_internal_store.h"
#include "rag_internal_store.h"
#include "rules_internal_store.h"
#include "schedule_tasks_internal_store.h"

WAVE_NAMESPACE_BEGIN
WEB_NAMESPACE_BEGIN
namespace internal {
namespace
{
    std::optional<int64_t> parseInt64Param(const std::string& raw)
    {
        if (raw.empty())
            return std::nullopt;
        try
        {
            return std::stoll(raw);
        }
        catch (const std::exception&)
        {
            return std::nullopt;
        }
    }

    std::optional<bool> parseBoolParam(const std::string& raw)
    {
        if (raw == "true" || raw == "1")
            return true;
        if (raw == "false" || raw == "0")
            return false;
        return std::nullopt;
    }

    drogon::orm::DbClientPtr requireDb(
        const std::function<void(const drogon::HttpResponsePtr&)>& callback)
    {
        auto& state = AppState::get();
        if (!state.db())
        {
            v1::respondError(callback, 503, "DB_UNAVAILABLE", "데이터베이스를 사용할 수 없습니다.");
            return nullptr;
        }
        return state.db();
    }

    int mapDeviceErrorStatus(const std::string& code)
    {
        if (code == "NOT_FOUND" || code == "ACTION_NOT_FOUND" || code == "QUERY_NOT_FOUND")
            return 404;
        if (code == "DEVICE_OFFLINE" || code == "DEVICE_INITIALIZING" || code == "AMBIGUOUS_DEVICE")
            return 409;
        if (code == "DEVICE_TIMEOUT")
            return 504;
        if (code == "DEVICES_UNAVAILABLE" || code == "AUTOMATION_UNAVAILABLE")
            return 503;
        if (code == "INVALID_REQUEST" || code == "INVALID_PARAMS" || code == "INVALID_EXEC_MODE")
            return 400;
        if (code == "RULE_DISABLED" || code == "COOLDOWN_ACTIVE")
            return 409;
        return 500;
    }

    std::optional<std::string> resolveDevicePathId(
        const drogon::orm::DbClientPtr& client,
        const std::string& device_id)
    {
        return DevicesInternalStore::resolveWireDeviceId(client, device_id);
    }

    void respondDeviceError(
        const std::function<void(const drogon::HttpResponsePtr&)>& callback,
        const std::string& code,
        const std::string& message)
    {
        v1::respondError(callback, mapDeviceErrorStatus(code), code, message);
    }

    void enrichDemoRuntimeBody(const drogon::HttpRequestPtr& req, Json::Value& body)
    {
        if (!demoVirtualDevicesEnabled())
            return;
        if (!body.isMember("demoRuntimeId") || !body["demoRuntimeId"].isString() ||
            body["demoRuntimeId"].asString().empty())
        {
            body["demoRuntimeId"] = resolveDemoRuntimeId(req, &body);
            return;
        }
        rememberPreferredDemoRuntimeId(body["demoRuntimeId"].asString());
    }

    void attachDemoRuntimeCookie(
        const drogon::HttpRequestPtr& req,
        const drogon::HttpResponsePtr& resp,
        const Json::Value& body)
    {
        if (!demoVirtualDevicesEnabled() || !resp)
            return;
        const auto runtime_id = body.isMember("demoRuntimeId") && body["demoRuntimeId"].isString()
            ? body["demoRuntimeId"].asString()
            : resolveDemoRuntimeId(req, nullptr);
        attachDemoRuntimeCookieIfNeeded(req, resp, runtime_id);
    }

    int mapCameraErrorStatus(const std::string& code)
    {
        if (code == "NOT_FOUND" || code == "UNSUPPORTED_DEVICE")
            return 404;
        if (code == "DEVICE_OFFLINE" || code == "DEVICE_INITIALIZING" || code == "STREAM_UNAVAILABLE")
            return 409;
        if (code == "INVALID_BODY")
            return 400;
        if (code == "TTS_UNAVAILABLE")
            return 503;
        return 500;
    }

    bool requireIotDevices(
        const std::function<void(const drogon::HttpResponsePtr&)>& callback,
        v1::IotStore& store)
    {
        if (!store.devicesAvailable())
        {
            v1::respondError(callback, 503, "DEVICES_UNAVAILABLE", "장치 관리자를 사용할 수 없습니다.");
            return false;
        }
        return true;
    }

    std::vector<std::string> splitCsv(const std::string& raw)
    {
        std::vector<std::string> parts;
        std::stringstream stream(raw);
        std::string item;
        while (std::getline(stream, item, ','))
        {
            if (!item.empty())
                parts.push_back(item);
        }
        return parts;
    }
}

void InternalController::queryDb(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    const auto client = requireDb(callback);
    if (!client)
        return;

    const auto json = req->getJsonObject();
    if (!json)
    {
        v1::respondError(callback, 400, "INVALID_BODY", "요청 본문이 필요합니다.");
        return;
    }

    DbQueryStore store(client);
    std::string error;
    std::string field;
    const auto body = store.execute(*json, error, field);
    if (body.isNull())
    {
        v1::respondError(callback, 400, "INVALID_REQUEST", error, field);
        return;
    }

    callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

void InternalController::searchRag(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    const auto client = requireDb(callback);
    if (!client)
        return;

    const auto json = req->getJsonObject();
    if (!json)
    {
        v1::respondError(callback, 400, "INVALID_BODY", "요청 본문이 필요합니다.");
        return;
    }

    const auto& config = AppState::get().config.agent;
    RagSearchConfig rag_config;
    rag_config.agent_base_url = config.base_url;
    rag_config.embedding_model = config.embedding_model;

    RagInternalStore store(client, rag_config);
    std::string error;
    std::string field;
    const auto body = store.search(*json, error, field);
    if (body.isNull())
    {
        v1::respondError(callback, 400, "INVALID_REQUEST", error, field);
        return;
    }

    callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

void InternalController::listDevices(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    const auto client = requireDb(callback);
    if (!client)
        return;

    DeviceListFilter filter;
    if (const auto user_id = parseInt64Param(req->getParameter("userId")))
        filter.user_id = *user_id;
    if (const auto room_id = parseInt64Param(req->getParameter("roomId")))
        filter.room_id = *room_id;
    if (!req->getParameter("class").empty())
        filter.device_class = req->getParameter("class");
    if (const auto connected = parseBoolParam(req->getParameter("connected")))
        filter.connected = *connected;
    if (const auto enabled = parseBoolParam(req->getParameter("enabled")))
        filter.enabled = *enabled;
    else
        filter.enabled = true;

    DevicesInternalStore store(client);
    std::string code;
    const auto runtime_id = demoVirtualDevicesEnabled()
        ? std::optional<std::string>(resolveDemoRuntimeId(req, nullptr))
        : std::nullopt;
    const auto body = store.listDevices(filter, code, runtime_id);
    if (!code.empty())
        respondDeviceError(callback, code, "장치 목록을 조회할 수 없습니다.");
    else
    {
        auto resp = drogon::HttpResponse::newHttpJsonResponse(body);
        if (runtime_id)
            attachDemoRuntimeCookieIfNeeded(req, resp, *runtime_id);
        callback(resp);
    }
}

void InternalController::getDevice(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string deviceId)
{
    const auto client = requireDb(callback);
    if (!client)
        return;

    std::optional<int64_t> user_id;
    if (const auto parsed = parseInt64Param(req->getParameter("userId")))
        user_id = *parsed;

    DevicesInternalStore store(client);
    std::string code;
    const auto runtime_id = demoVirtualDevicesEnabled()
        ? std::optional<std::string>(resolveDemoRuntimeId(req, nullptr))
        : std::nullopt;
    const auto body = store.getDevice(deviceId, user_id, code, runtime_id);
    if (!code.empty())
        respondDeviceError(callback, code, "기기를 찾을 수 없습니다.");
    else
    {
        auto resp = drogon::HttpResponse::newHttpJsonResponse(body);
        if (runtime_id)
            attachDemoRuntimeCookieIfNeeded(req, resp, *runtime_id);
        callback(resp);
    }
}

void InternalController::listDeviceClasses(
    const drogon::HttpRequestPtr& /*req*/,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    callback(drogon::HttpResponse::newHttpJsonResponse(DeviceClassRegistry::listClasses()));
}

void InternalController::getDeviceState(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string deviceId)
{
    const auto client = requireDb(callback);
    if (!client)
        return;

    std::optional<int64_t> user_id;
    if (const auto parsed = parseInt64Param(req->getParameter("userId")))
        user_id = *parsed;

    DevicesInternalStore store(client);
    std::string code;
    const auto runtime_id = demoVirtualDevicesEnabled()
        ? std::optional<std::string>(resolveDemoRuntimeId(req, nullptr))
        : std::nullopt;
    const auto body = store.getState(deviceId, user_id, code, runtime_id);
    if (!code.empty())
        respondDeviceError(callback, code, "상태 조회에 실패했습니다.");
    else
    {
        auto resp = drogon::HttpResponse::newHttpJsonResponse(body);
        if (runtime_id)
            attachDemoRuntimeCookieIfNeeded(req, resp, *runtime_id);
        callback(resp);
    }
}

void InternalController::queryDevice(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string deviceId,
    std::string queryName)
{
    const auto client = requireDb(callback);
    if (!client)
        return;

    Json::Value body(Json::objectValue);
    const auto json = req->getJsonObject();
    if (json && json->isObject())
        body = *json;
    enrichDemoRuntimeBody(req, body);

    DevicesInternalStore store(client);
    std::string code;
    const auto response = store.queryDevice(deviceId, queryName, body, code);
    if (!code.empty())
        respondDeviceError(callback, code, "조회에 실패했습니다.");
    else
    {
        auto resp = drogon::HttpResponse::newHttpJsonResponse(response);
        attachDemoRuntimeCookie(req, resp, body);
        callback(resp);
    }
}

void InternalController::invokeDeviceAction(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string deviceId,
    std::string actionName)
{
    const auto client = requireDb(callback);
    if (!client)
        return;

    Json::Value body(Json::objectValue);
    const auto json = req->getJsonObject();
    if (json && json->isObject())
        body = *json;
    enrichDemoRuntimeBody(req, body);

    DevicesInternalStore store(client);
    std::string code;
    const auto response = store.invokeAction(deviceId, actionName, body, code);
    if (!code.empty())
        respondDeviceError(callback, code, "제어에 실패했습니다.");
    else
    {
        auto resp = drogon::HttpResponse::newHttpJsonResponse(response);
        attachDemoRuntimeCookie(req, resp, body);
        callback(resp);
    }
}

void InternalController::getPtzCapabilities(
    const drogon::HttpRequestPtr& /*req*/,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string deviceId)
{
    const auto client = requireDb(callback);
    if (!client)
        return;

    const auto manifest_id = resolveDevicePathId(client, deviceId);
    if (!manifest_id)
    {
        respondDeviceError(callback, "NOT_FOUND", "기기를 찾을 수 없습니다.");
        return;
    }

    auto& state = AppState::get();
    v1::IotStore store(state.deviceManager);
    if (!requireIotDevices(callback, store))
        return;

    std::string code;
    const auto body = store.getPtzCapabilities(*manifest_id, code);
    if (!code.empty())
        v1::respondError(callback, mapCameraErrorStatus(code), code, "PTZ 기능을 조회할 수 없습니다.");
    else
        callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

void InternalController::movePtz(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string deviceId)
{
    const auto client = requireDb(callback);
    if (!client)
        return;

    const auto manifest_id = resolveDevicePathId(client, deviceId);
    if (!manifest_id)
    {
        respondDeviceError(callback, "NOT_FOUND", "기기를 찾을 수 없습니다.");
        return;
    }

    auto& state = AppState::get();
    v1::IotStore store(state.deviceManager);
    if (!requireIotDevices(callback, store))
        return;

    const auto json = req->getJsonObject();
    Json::Value vector(Json::objectValue);
    if (json && json->isObject())
        vector = *json;

    std::string code;
    if (!store.moveCameraPtz(*manifest_id, vector, code))
        v1::respondError(callback, mapCameraErrorStatus(code), code, "PTZ 이동에 실패했습니다.");
    else
    {
        Json::Value body;
        body["ok"] = true;
        callback(drogon::HttpResponse::newHttpJsonResponse(body));
    }
}

void InternalController::stopPtz(
    const drogon::HttpRequestPtr& /*req*/,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string deviceId)
{
    const auto client = requireDb(callback);
    if (!client)
        return;

    const auto manifest_id = resolveDevicePathId(client, deviceId);
    if (!manifest_id)
    {
        respondDeviceError(callback, "NOT_FOUND", "기기를 찾을 수 없습니다.");
        return;
    }

    auto& state = AppState::get();
    v1::IotStore store(state.deviceManager);
    if (!requireIotDevices(callback, store))
        return;

    std::string code;
    if (!store.stopCameraPtz(*manifest_id, code))
        v1::respondError(callback, mapCameraErrorStatus(code), code, "PTZ 정지에 실패했습니다.");
    else
    {
        Json::Value body;
        body["ok"] = true;
        callback(drogon::HttpResponse::newHttpJsonResponse(body));
    }
}

void InternalController::zoomPtz(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string deviceId)
{
    const auto client = requireDb(callback);
    if (!client)
        return;

    const auto manifest_id = resolveDevicePathId(client, deviceId);
    if (!manifest_id)
    {
        respondDeviceError(callback, "NOT_FOUND", "기기를 찾을 수 없습니다.");
        return;
    }

    auto& state = AppState::get();
    v1::IotStore store(state.deviceManager);
    if (!requireIotDevices(callback, store))
        return;

    const auto json = req->getJsonObject();
    const int delta = json && json->isMember("delta") ? (*json)["delta"].asInt() : 0;

    std::string code;
    const auto body = store.zoomCameraPtz(*manifest_id, delta, code);
    if (!code.empty())
        v1::respondError(callback, mapCameraErrorStatus(code), code, "줌 조정에 실패했습니다.");
    else
        callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

void InternalController::getCameraStream(
    const drogon::HttpRequestPtr& /*req*/,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string deviceId)
{
    const auto client = requireDb(callback);
    if (!client)
        return;

    const auto manifest_id = resolveDevicePathId(client, deviceId);
    if (!manifest_id)
    {
        respondDeviceError(callback, "NOT_FOUND", "기기를 찾을 수 없습니다.");
        return;
    }

    auto& state = AppState::get();
    v1::IotStore store(state.deviceManager);
    if (!requireIotDevices(callback, store))
        return;

    std::string code;
    const auto body = store.getCameraStream(*manifest_id, code);
    if (!code.empty())
        v1::respondError(callback, mapCameraErrorStatus(code), code, "스트림 정보를 가져올 수 없습니다.");
    else
        callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

void InternalController::setCameraStream(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string deviceId)
{
    const auto client = requireDb(callback);
    if (!client)
        return;

    const auto manifest_id = resolveDevicePathId(client, deviceId);
    if (!manifest_id)
    {
        respondDeviceError(callback, "NOT_FOUND", "기기를 찾을 수 없습니다.");
        return;
    }

    auto& state = AppState::get();
    v1::IotStore store(state.deviceManager);
    if (!requireIotDevices(callback, store))
        return;

    const auto json = req->getJsonObject();
    if (!json || !json->isMember("streaming"))
    {
        v1::respondError(callback, 400, "INVALID_BODY", "streaming 필드가 필요합니다.");
        return;
    }

    std::string code;
    const auto body = store.setCameraStream(*manifest_id, (*json)["streaming"].asBool(), code);
    if (!code.empty())
        v1::respondError(callback, mapCameraErrorStatus(code), code, "스트림을 시작할 수 없습니다.");
    else
        callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

void InternalController::captureSnapshot(
    const drogon::HttpRequestPtr& /*req*/,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string deviceId)
{
    const auto client = requireDb(callback);
    if (!client)
        return;

    const auto manifest_id = resolveDevicePathId(client, deviceId);
    if (!manifest_id)
    {
        respondDeviceError(callback, "NOT_FOUND", "기기를 찾을 수 없습니다.");
        return;
    }

    auto& state = AppState::get();
    v1::IotStore store(state.deviceManager);
    if (!requireIotDevices(callback, store))
        return;

    std::thread([manifest_id = *manifest_id, callback = std::move(callback), &state]() mutable
    {
        v1::IotStore worker(state.deviceManager);
        std::vector<uint8_t> jpeg;
        std::string occurred_at;
        std::string code;
        if (!worker.captureCameraSnapshot(manifest_id, jpeg, occurred_at, code))
        {
            v1::respondError(callback, mapCameraErrorStatus(code), code, "스냅샷 캡처에 실패했습니다.");
            return;
        }

        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setContentTypeCode(drogon::ContentType::CT_IMAGE_JPG);
        resp->setBody(std::string(jpeg.begin(), jpeg.end()));
        resp->addHeader("X-Snapshot-At", occurred_at);
        callback(resp);
    }).detach();
}

void InternalController::sendTts(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string deviceId)
{
    const auto client = requireDb(callback);
    if (!client)
        return;

    const auto manifest_id = resolveDevicePathId(client, deviceId);
    if (!manifest_id)
    {
        respondDeviceError(callback, "NOT_FOUND", "기기를 찾을 수 없습니다.");
        return;
    }

    auto& state = AppState::get();
    v1::IotStore store(state.deviceManager);
    if (!requireIotDevices(callback, store))
        return;

    const auto json = req->getJsonObject();
    if (!json || !json->isMember("text"))
    {
        v1::respondError(callback, 400, "INVALID_BODY", "text 필드가 필요합니다.");
        return;
    }

    const std::string text = (*json)["text"].asString();
    const int speaker_id = json->isMember("speakerId") ? (*json)["speakerId"].asInt() : 0;
    const float speed = json->isMember("speed") ? static_cast<float>((*json)["speed"].asDouble()) : 1.0f;

    v1::queueDeviceTts(*manifest_id, text, speaker_id, speed);
    Json::Value body;
    body["ok"] = true;
    callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

void InternalController::listRules(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    RuleListFilter filter;
    if (!req->getParameter("deviceId").empty())
        filter.device_id = req->getParameter("deviceId");
    if (const auto enabled = parseBoolParam(req->getParameter("enabled")))
        filter.enabled = *enabled;
    if (const auto has_schedule = parseBoolParam(req->getParameter("hasSchedule")))
        filter.has_schedule = *has_schedule;
    if (const auto has_trigger = parseBoolParam(req->getParameter("hasTrigger")))
        filter.has_trigger = *has_trigger;

    RulesInternalStore store;
    std::string code;
    if (demoVirtualDevicesEnabled())
    {
        const auto client = AppState::get().db();
        const auto runtime_id = resolveDemoRuntimeId(req, nullptr);
        ensureDemoSessionSeeded(runtime_id, client);
        Json::Value body;
        body["items"] = demoListRules(runtime_id, 0);
        body["count"] = static_cast<Json::UInt>(body["items"].size());
        auto resp = drogon::HttpResponse::newHttpJsonResponse(body);
        attachDemoRuntimeCookieIfNeeded(req, resp, runtime_id);
        callback(resp);
        return;
    }

    const auto body = store.listRules(filter, code);
    if (!code.empty())
        respondDeviceError(callback, code, "룰 목록을 조회할 수 없습니다.");
    else
        callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

void InternalController::getRule(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string ruleId)
{
    if (demoVirtualDevicesEnabled())
    {
        const auto runtime_id = resolveDemoRuntimeId(req, nullptr);
        ensureDemoSessionSeeded(runtime_id, AppState::get().db());
        for (const auto& item : demoListRules(runtime_id, 0))
        {
            if (item.isObject() && item.get("id", "").asString() == ruleId)
            {
                auto resp = drogon::HttpResponse::newHttpJsonResponse(item);
                attachDemoRuntimeCookieIfNeeded(req, resp, runtime_id);
                callback(resp);
                return;
            }
        }
        respondDeviceError(callback, "NOT_FOUND", "룰을 찾을 수 없습니다.");
        return;
    }

    RulesInternalStore store;
    std::string code;
    const auto body = store.getRule(ruleId, code);
    if (!code.empty())
        respondDeviceError(callback, code, "룰을 찾을 수 없습니다.");
    else
        callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

void InternalController::createRule(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    const auto json = req->getJsonObject();
    if (!json)
    {
        v1::respondError(callback, 400, "INVALID_BODY", "요청 본문이 필요합니다.");
        return;
    }

    RulesInternalStore store;
    std::string code;
    if (demoVirtualDevicesEnabled())
    {
        Json::Value body = *json;
        enrichDemoRuntimeBody(req, body);
        const auto runtime_id = body["demoRuntimeId"].asString();
        ensureDemoSessionSeeded(runtime_id, AppState::get().db());
        const auto created = demoCreateRule(runtime_id, body, code);
        if (created.isNull())
            respondDeviceError(callback, code.empty() ? "INVALID_REQUEST" : code, "룰을 생성할 수 없습니다.");
        else
        {
            auto resp = drogon::HttpResponse::newHttpJsonResponse(created);
            resp->setStatusCode(drogon::k201Created);
            attachDemoRuntimeCookie(req, resp, body);
            callback(resp);
        }
        return;
    }

    const auto body = store.createRule(*json, code);
    if (!code.empty())
        respondDeviceError(callback, code, "룰을 생성할 수 없습니다.");
    else
    {
        auto resp = drogon::HttpResponse::newHttpJsonResponse(body);
        resp->setStatusCode(drogon::k201Created);
        callback(resp);
    }
}

void InternalController::updateRule(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string ruleId)
{
    const auto json = req->getJsonObject();
    if (!json)
    {
        v1::respondError(callback, 400, "INVALID_BODY", "요청 본문이 필요합니다.");
        return;
    }

    if (demoVirtualDevicesEnabled())
    {
        Json::Value body = *json;
        enrichDemoRuntimeBody(req, body);
        const auto runtime_id = body["demoRuntimeId"].asString();
        ensureDemoSessionSeeded(runtime_id, AppState::get().db());
        std::string code;
        const auto updated = demoUpdateRule(runtime_id, ruleId, body, code);
        if (!code.empty())
            respondDeviceError(callback, code, "룰을 수정할 수 없습니다.");
        else
        {
            auto resp = drogon::HttpResponse::newHttpJsonResponse(updated);
            attachDemoRuntimeCookie(req, resp, body);
            callback(resp);
        }
        return;
    }

    RulesInternalStore store;
    std::string code;
    const auto body = store.updateRule(ruleId, *json, code);
    if (!code.empty())
        respondDeviceError(callback, code, "룰을 수정할 수 없습니다.");
    else
        callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

void InternalController::deleteRule(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string ruleId)
{
    if (demoVirtualDevicesEnabled())
    {
        const auto runtime_id = resolveDemoRuntimeId(req, nullptr);
        ensureDemoSessionSeeded(runtime_id, AppState::get().db());
        if (!demoDeleteRule(runtime_id, ruleId))
        {
            respondDeviceError(callback, "NOT_FOUND", "룰을 삭제할 수 없습니다.");
            return;
        }
        Json::Value body;
        body["id"] = ruleId;
        auto resp = drogon::HttpResponse::newHttpJsonResponse(body);
        attachDemoRuntimeCookieIfNeeded(req, resp, runtime_id);
        callback(resp);
        return;
    }

    RulesInternalStore store;
    std::string code;
    const auto body = store.deleteRule(ruleId, code);
    if (!code.empty())
        respondDeviceError(callback, code, "룰을 삭제할 수 없습니다.");
    else
        callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

void InternalController::setRuleEnabled(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string ruleId)
{
    const auto json = req->getJsonObject();
    if (!json || !json->isMember("enabled") || !(*json)["enabled"].isBool())
    {
        v1::respondError(callback, 400, "INVALID_REQUEST", "enabled 값이 필요합니다.", "enabled");
        return;
    }

    if (demoVirtualDevicesEnabled())
    {
        Json::Value body;
        body["enabled"] = (*json)["enabled"].asBool();
        enrichDemoRuntimeBody(req, body);
        const auto runtime_id = body["demoRuntimeId"].asString();
        ensureDemoSessionSeeded(runtime_id, AppState::get().db());
        std::string code;
        const auto updated = demoUpdateRule(runtime_id, ruleId, body, code);
        if (!code.empty())
            respondDeviceError(callback, code, "룰 상태를 변경할 수 없습니다.");
        else
        {
            auto resp = drogon::HttpResponse::newHttpJsonResponse(updated);
            attachDemoRuntimeCookie(req, resp, body);
            callback(resp);
        }
        return;
    }

    RulesInternalStore store;
    std::string code;
    const auto body = store.setRuleEnabled(ruleId, (*json)["enabled"].asBool(), code);
    if (!code.empty())
        respondDeviceError(callback, code, "룰 상태를 변경할 수 없습니다.");
    else
        callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

void InternalController::executeRule(
    const drogon::HttpRequestPtr& /*req*/,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string ruleId)
{
    RulesInternalStore store;
    std::string code;
    const auto body = store.executeRule(ruleId, code);
    if (!code.empty())
        respondDeviceError(callback, code, "룰 실행에 실패했습니다.");
    else
        callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

void InternalController::listIrCommands(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    auto& state = AppState::get();
    if (!state.hasIrStore())
    {
        v1::respondError(callback, 503, "IR_STORE_UNAVAILABLE", "IR 저장소를 사용할 수 없습니다.");
        return;
    }

    const auto device_hint = req->getParameter("deviceHint");
    const auto source = req->getParameter("source");
    const Json::Value listed = state.irStore().listCommands();

    Json::Value items(Json::arrayValue);
    for (const auto& item : listed)
    {
        if (!item.isObject())
            continue;
        if (!device_hint.empty() && item.get("deviceHint", "").asString() != device_hint)
            continue;
        if (!source.empty() && item.get("source", "").asString() != source)
            continue;

        Json::Value summary = item;
        summary.removeMember("timings");
        items.append(summary);
    }

    Json::Value body;
    body["items"] = items;
    body["count"] = static_cast<Json::UInt>(items.size());
    callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

void InternalController::getIrCommand(
    const drogon::HttpRequestPtr& /*req*/,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string commandId)
{
    auto& state = AppState::get();
    if (!state.hasIrStore())
    {
        v1::respondError(callback, 503, "IR_STORE_UNAVAILABLE", "IR 저장소를 사용할 수 없습니다.");
        return;
    }

    std::string code;
    const auto body = state.irStore().getCommand(commandId, code);
    if (code == "NOT_FOUND")
        v1::respondError(callback, 404, code, "IR 커맨드를 찾을 수 없습니다.");
    else if (!code.empty())
        v1::respondError(callback, 500, code, "IR 커맨드를 조회할 수 없습니다.");
    else
        callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

void InternalController::listEvents(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    const auto client = requireDb(callback);
    if (!client)
        return;

    EventsListFilter filter;
    if (!req->getParameter("types").empty())
        filter.types = splitCsv(req->getParameter("types"));
    if (!req->getParameter("deviceId").empty())
        filter.device_id = req->getParameter("deviceId");
    if (!req->getParameter("from").empty())
        filter.from = req->getParameter("from");
    if (!req->getParameter("to").empty())
        filter.to = req->getParameter("to");
    if (const auto limit = parseInt64Param(req->getParameter("limit")))
        filter.limit = static_cast<int>(*limit);

    DevicesInternalStore store(client);
    std::string code;
    const auto body = store.listEvents(filter, code);
    if (!code.empty())
        respondDeviceError(callback, code, "이벤트를 조회할 수 없습니다.");
    else
        callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

void InternalController::toolListDevices(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    const auto client = requireDb(callback);
    if (!client)
        return;

    const auto json = req->getJsonObject();
    if (!json)
    {
        v1::respondError(callback, 400, "INVALID_BODY", "요청 본문이 필요합니다.");
        return;
    }

    DevicesInternalStore store(client);
    std::string code;
    const auto body = store.toolListDevices(*json, code);
    if (!code.empty())
        respondDeviceError(callback, code, "장치 목록을 조회할 수 없습니다.");
    else
        callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

void InternalController::toolControlDevice(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    const auto client = requireDb(callback);
    if (!client)
        return;

    const auto json = req->getJsonObject();
    if (!json)
    {
        v1::respondError(callback, 400, "INVALID_BODY", "요청 본문이 필요합니다.");
        return;
    }

    Json::Value body = *json;
    enrichDemoRuntimeBody(req, body);

    DevicesInternalStore store(client);
    std::string code;
    const auto response = store.toolControlDevice(body, code);
    if (!code.empty())
        respondDeviceError(callback, code, "제어에 실패했습니다.");
    else
    {
        auto resp = drogon::HttpResponse::newHttpJsonResponse(response);
        attachDemoRuntimeCookie(req, resp, body);
        callback(resp);
    }
}

void InternalController::toolQueryDevice(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    const auto client = requireDb(callback);
    if (!client)
        return;

    const auto json = req->getJsonObject();
    if (!json)
    {
        v1::respondError(callback, 400, "INVALID_BODY", "요청 본문이 필요합니다.");
        return;
    }

    DevicesInternalStore store(client);
    std::string code;
    const auto body = store.toolQueryDevice(*json, code);
    if (!code.empty())
        respondDeviceError(callback, code, "조회에 실패했습니다.");
    else
        callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

void InternalController::toolSchedule(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    const auto json = req->getJsonObject();
    if (!json)
    {
        v1::respondError(callback, 400, "INVALID_BODY", "요청 본문이 필요합니다.");
        return;
    }

    RulesInternalStore store;
    std::string code;
    const auto body = store.toolSchedule(*json, code);
    if (!code.empty())
        respondDeviceError(callback, code, "예약을 생성할 수 없습니다.");
    else
    {
        auto resp = drogon::HttpResponse::newHttpJsonResponse(body);
        resp->setStatusCode(drogon::k201Created);
        callback(resp);
    }
}

void InternalController::toolScheduleList(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    const auto json = req->getJsonObject();
    if (!json)
    {
        v1::respondError(callback, 400, "INVALID_BODY", "요청 본문이 필요합니다.");
        return;
    }

    RulesInternalStore store;
    std::string code;
    const auto body = store.toolScheduleList(*json, code);
    if (!code.empty())
        respondDeviceError(callback, code, "예약 목록을 조회할 수 없습니다.");
    else
        callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

void InternalController::toolScheduleCancel(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    const auto json = req->getJsonObject();
    if (!json)
    {
        v1::respondError(callback, 400, "INVALID_BODY", "요청 본문이 필요합니다.");
        return;
    }

    RulesInternalStore store;
    std::string code;
    const auto body = store.toolScheduleCancel(*json, code);
    if (!code.empty())
        respondDeviceError(callback, code, "예약을 취소할 수 없습니다.");
    else
        callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

void InternalController::listAlarms(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    const auto client = requireDb(callback);
    if (!client)
        return;

    const auto user_id = parseInt64Param(req->getParameter("userId"));
    if (!user_id)
    {
        v1::respondError(callback, 400, "INVALID_REQUEST", "userId가 필요합니다.", "userId");
        return;
    }

    AlarmListFilter filter;
    filter.user_id = *user_id;
    if (const auto enabled = parseBoolParam(req->getParameter("enabled")))
        filter.enabled = *enabled;

    AlarmsInternalStore store(client);
    if (demoVirtualDevicesEnabled())
    {
        const auto runtime_id = resolveDemoRuntimeId(req, nullptr);
        std::optional<bool> enabled_filter;
        if (const auto enabled = parseBoolParam(req->getParameter("enabled")))
            enabled_filter = *enabled;
        auto resp = drogon::HttpResponse::newHttpJsonResponse(
            demoListAlarms(runtime_id, *user_id, client, enabled_filter));
        attachDemoRuntimeCookieIfNeeded(req, resp, runtime_id);
        callback(resp);
        return;
    }
    callback(drogon::HttpResponse::newHttpJsonResponse(store.listAlarms(filter)));
}

void InternalController::createAlarm(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    const auto client = requireDb(callback);
    if (!client)
        return;

    const auto json = req->getJsonObject();
    if (!json)
    {
        v1::respondError(callback, 400, "INVALID_BODY", "요청 본문이 필요합니다.");
        return;
    }

    AlarmsInternalStore store(client);
    std::string error;
    std::string field;
    if (demoVirtualDevicesEnabled())
    {
        Json::Value body = *json;
        enrichDemoRuntimeBody(req, body);
        const auto runtime_id = body["demoRuntimeId"].asString();
        const auto created = demoCreateAlarm(runtime_id, body, client, error, field);
        if (created.isNull())
            v1::respondError(callback, 400, "VALIDATION_ERROR", error, field);
        else
        {
            auto resp = drogon::HttpResponse::newHttpJsonResponse(created);
            resp->setStatusCode(drogon::k201Created);
            attachDemoRuntimeCookie(req, resp, body);
            callback(resp);
        }
        return;
    }

    const auto created = store.createAlarm(*json, error, field);
    if (created.isNull())
        v1::respondError(callback, 400, "VALIDATION_ERROR", error, field);
    else
    {
        service::AlarmManager::get().reconcile();
        auto resp = drogon::HttpResponse::newHttpJsonResponse(created);
        resp->setStatusCode(drogon::k201Created);
        callback(resp);
    }
}

void InternalController::updateAlarm(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string id)
{
    const auto client = requireDb(callback);
    if (!client)
        return;

    const auto user_id = parseInt64Param(req->getParameter("userId"));
    const auto alarm_id = parseInt64Param(id);
    if (!user_id)
    {
        v1::respondError(callback, 400, "INVALID_REQUEST", "userId가 필요합니다.", "userId");
        return;
    }
    if (!alarm_id)
    {
        v1::respondError(callback, 404, "NOT_FOUND", "알람을 찾을 수 없습니다.");
        return;
    }

    const auto json = req->getJsonObject();
    if (!json)
    {
        v1::respondError(callback, 400, "INVALID_BODY", "요청 본문이 필요합니다.");
        return;
    }

    std::string error;
    std::string field;
    if (demoVirtualDevicesEnabled())
    {
        const auto runtime_id = resolveDemoRuntimeId(req, json.get());
        const auto updated = demoUpdateAlarm(runtime_id, *alarm_id, *json, error, field);
        if (updated.isNull())
        {
            const int status = error.find("찾을") != std::string::npos ? 404 : 400;
            v1::respondError(callback, status, status == 404 ? "NOT_FOUND" : "VALIDATION_ERROR", error, field);
            return;
        }
        auto resp = drogon::HttpResponse::newHttpJsonResponse(updated);
        attachDemoRuntimeCookieIfNeeded(req, resp, runtime_id);
        callback(resp);
        return;
    }

    AlarmsInternalStore store(client);
    const auto updated = store.updateAlarm(*user_id, *alarm_id, *json, error, field);
    if (updated.isNull())
    {
        const int status = error.find("찾을") != std::string::npos ? 404 : 400;
        v1::respondError(callback, status, status == 404 ? "NOT_FOUND" : "VALIDATION_ERROR", error, field);
    }
    else
    {
        service::AlarmManager::get().reconcile();
        callback(drogon::HttpResponse::newHttpJsonResponse(updated));
    }
}

void InternalController::deleteAlarm(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string id)
{
    const auto client = requireDb(callback);
    if (!client)
        return;

    const auto user_id = parseInt64Param(req->getParameter("userId"));
    const auto alarm_id = parseInt64Param(id);
    if (!user_id)
    {
        v1::respondError(callback, 400, "INVALID_REQUEST", "userId가 필요합니다.", "userId");
        return;
    }
    if (!alarm_id)
    {
        v1::respondError(callback, 404, "NOT_FOUND", "알람을 찾을 수 없습니다.");
        return;
    }

    if (demoVirtualDevicesEnabled())
    {
        const auto runtime_id = resolveDemoRuntimeId(req, nullptr);
        if (!demoDeleteAlarm(runtime_id, *alarm_id))
        {
            v1::respondError(callback, 404, "NOT_FOUND", "세션 알람을 찾을 수 없습니다.");
            return;
        }
        Json::Value removed;
        removed["id"] = static_cast<Json::Int64>(*alarm_id);
        auto resp = drogon::HttpResponse::newHttpJsonResponse(removed);
        attachDemoRuntimeCookieIfNeeded(req, resp, runtime_id);
        callback(resp);
        return;
    }

    AlarmsInternalStore store(client);
    std::string error;
    const auto removed = store.deleteAlarm(*user_id, *alarm_id, error);
    if (removed.isNull())
        v1::respondError(callback, 404, "NOT_FOUND", error);
    else
    {
        service::AlarmManager::get().reconcile();
        callback(drogon::HttpResponse::newHttpJsonResponse(removed));
    }
}

void InternalController::listScheduleTasks(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    const auto client = requireDb(callback);
    if (!client)
        return;

    const auto user_id = parseInt64Param(req->getParameter("userId"));
    if (!user_id)
    {
        v1::respondError(callback, 400, "INVALID_REQUEST", "userId 가 필요합니다.", "userId");
        return;
    }

    ScheduleTaskListFilter filter;
    filter.user_id = *user_id;
    if (!req->getParameter("dayOfWeek").empty())
        filter.day_of_week = req->getParameter("dayOfWeek");
    if (!req->getParameter("eventDate").empty())
        filter.event_date = req->getParameter("eventDate");
    if (!req->getParameter("scheduleKind").empty())
        filter.schedule_kind = req->getParameter("scheduleKind");
    if (!req->getParameter("from").empty())
        filter.from = req->getParameter("from");
    if (!req->getParameter("to").empty())
        filter.to = req->getParameter("to");
    if (const auto done = parseBoolParam(req->getParameter("done")))
        filter.done = *done;

    ScheduleTasksInternalStore store(client);
    if (demoVirtualDevicesEnabled())
    {
        const auto runtime_id = resolveDemoRuntimeId(req, nullptr);
        ensureDemoSessionSeeded(runtime_id, client);
        auto resp = drogon::HttpResponse::newHttpJsonResponse(
            demoListScheduleTasks(runtime_id, *user_id, client));
        attachDemoRuntimeCookieIfNeeded(req, resp, runtime_id);
        callback(resp);
        return;
    }
    callback(drogon::HttpResponse::newHttpJsonResponse(store.list(filter)));
}

void InternalController::createScheduleTask(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    const auto client = requireDb(callback);
    if (!client)
        return;

    const auto json = req->getJsonObject();
    if (!json)
    {
        v1::respondError(callback, 400, "INVALID_BODY", "요청 본문이 필요합니다.");
        return;
    }

    ScheduleTasksInternalStore store(client);
    std::string error;
    std::string field;
    if (demoVirtualDevicesEnabled())
    {
        Json::Value body = *json;
        enrichDemoRuntimeBody(req, body);
        const auto runtime_id = body["demoRuntimeId"].asString();
        ensureDemoSessionSeeded(runtime_id, client);
        const auto created = demoCreateScheduleTask(runtime_id, body, error, field);
        if (created.isNull())
        {
            v1::respondError(callback, 400, "INVALID_REQUEST", error, field);
            return;
        }
        auto resp = drogon::HttpResponse::newHttpJsonResponse(created);
        resp->setStatusCode(drogon::k201Created);
        attachDemoRuntimeCookie(req, resp, body);
        callback(resp);
        return;
    }

    const auto created = store.create(*json, error, field);
    if (!created)
    {
        v1::respondError(callback, 400, "INVALID_REQUEST", error, field);
        return;
    }

    auto resp = drogon::HttpResponse::newHttpJsonResponse(*created);
    resp->setStatusCode(drogon::k201Created);
    callback(resp);
}

void InternalController::updateScheduleTask(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string taskId)
{
    const auto client = requireDb(callback);
    if (!client)
        return;

    const auto user_id = parseInt64Param(req->getParameter("userId"));
    const auto id = parseInt64Param(taskId);
    if (!user_id)
    {
        v1::respondError(callback, 400, "INVALID_REQUEST", "userId 가 필요합니다.", "userId");
        return;
    }
    if (!id)
    {
        v1::respondError(callback, 404, "NOT_FOUND", "일정을 찾을 수 없습니다.");
        return;
    }

    const auto json = req->getJsonObject();
    if (!json)
    {
        v1::respondError(callback, 400, "INVALID_BODY", "요청 본문이 필요합니다.");
        return;
    }

    std::string error;
    std::string field;
    if (demoVirtualDevicesEnabled())
    {
        const auto runtime_id = resolveDemoRuntimeId(req, json.get());
        const auto updated = demoUpdateScheduleTask(runtime_id, *id, *json, error, field);
        if (updated.isNull())
        {
            const int status = error.find("찾을") != std::string::npos ? 404 : 400;
            v1::respondError(callback, status, status == 404 ? "NOT_FOUND" : "INVALID_REQUEST", error, field);
            return;
        }
        auto resp = drogon::HttpResponse::newHttpJsonResponse(updated);
        attachDemoRuntimeCookieIfNeeded(req, resp, runtime_id);
        callback(resp);
        return;
    }

    ScheduleTasksInternalStore store(client);
    const auto updated = store.update(*user_id, *id, *json, error, field);
    if (!updated)
    {
        const int status = error.find("찾을") != std::string::npos ? 404 : 400;
        const std::string code = status == 404 ? "NOT_FOUND" : "INVALID_REQUEST";
        v1::respondError(callback, status, code, error, field);
        return;
    }

    callback(drogon::HttpResponse::newHttpJsonResponse(*updated));
}

void InternalController::deleteScheduleTask(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string taskId)
{
    const auto client = requireDb(callback);
    if (!client)
        return;

    const auto user_id = parseInt64Param(req->getParameter("userId"));
    const auto id = parseInt64Param(taskId);
    if (!user_id)
    {
        v1::respondError(callback, 400, "INVALID_REQUEST", "userId 가 필요합니다.", "userId");
        return;
    }
    if (!id)
    {
        v1::respondError(callback, 404, "NOT_FOUND", "일정을 찾을 수 없습니다.");
        return;
    }

    if (demoVirtualDevicesEnabled())
    {
        const auto runtime_id = resolveDemoRuntimeId(req, nullptr);
        if (!demoDeleteScheduleTask(runtime_id, *id))
        {
            v1::respondError(callback, 404, "NOT_FOUND", "세션 일정을 찾을 수 없습니다.");
            return;
        }
        Json::Value removed;
        removed["id"] = static_cast<Json::Int64>(*id);
        auto resp = drogon::HttpResponse::newHttpJsonResponse(removed);
        attachDemoRuntimeCookieIfNeeded(req, resp, runtime_id);
        callback(resp);
        return;
    }

    ScheduleTasksInternalStore store(client);
    std::string error;
    const auto removed = store.remove(*user_id, *id, error);
    if (!removed)
    {
        v1::respondError(callback, 404, "NOT_FOUND", error);
        return;
    }

    callback(drogon::HttpResponse::newHttpJsonResponse(*removed));
}

} // namespace internal
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
