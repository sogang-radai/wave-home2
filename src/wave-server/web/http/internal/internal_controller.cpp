#include "internal_controller.h"
#include "../../../db/database.h"

#include <cstdint>
#include <optional>
#include <sstream>
#include <thread>
#include <vector>

#include "../../../app/app_state.h"
#include "../../../demo/demo_device_backend.h"
#include "../../../demo/demo_session_writes.h"
#include "../../../demo/demo_runtime_id.h"
#include "../../../facade/alarms_facade.h"
#include "../../../facade/schedule_tasks_facade.h"
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
    std::optional<int64_t> parse_int64_param(const std::string& raw)
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

    std::optional<bool> parse_bool_param(const std::string& raw)
    {
        if (raw == "true" || raw == "1")
            return true;
        if (raw == "false" || raw == "0")
            return false;
        return std::nullopt;
    }

    db::DbClientPtr require_db(
        const HttpResponseCallback& callback)
    {
        auto& state = AppState::get();
        if (!state.db())
        {
            v1::respondError(callback, 503, "DB_UNAVAILABLE", "데이터베이스를 사용할 수 없습니다.");
            return nullptr;
        }
        return state.db();
    }

    int map_device_error_status(const std::string& code)
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

    std::optional<std::string> resolve_device_path_id(
        const db::DbClientPtr& client,
        const std::string& device_id)
    {
        return DevicesInternalStore::resolve_wire_device_id(client, device_id);
    }

    void respond_device_error(
        const HttpResponseCallback& callback,
        const std::string& code,
        const std::string& message)
    {
        v1::respondError(callback, map_device_error_status(code), code, message);
    }

    void enrich_demo_runtime_body(const HttpRequestPtr& req, Json::Value& body)
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

    void attach_demo_runtime_cookie(
        const HttpRequestPtr& req,
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

    int map_camera_error_status(const std::string& code)
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

    bool require_iot_devices(
        const HttpResponseCallback& callback,
        v1::IotStore& store)
    {
        if (!store.devicesAvailable())
        {
            v1::respondError(callback, 503, "DEVICES_UNAVAILABLE", "장치 관리자를 사용할 수 없습니다.");
            return false;
        }
        return true;
    }

    std::vector<std::string> split_csv(const std::string& raw)
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

void InternalController::queryDb(const HttpRequestPtr& req, HttpResponseCallback&& callback)
{
    const auto client = require_db(callback);
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

void InternalController::searchRag(const HttpRequestPtr& req, HttpResponseCallback&& callback)
{
    const auto client = require_db(callback);
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

void InternalController::listDevices(const HttpRequestPtr& req, HttpResponseCallback&& callback)
{
    const auto client = require_db(callback);
    if (!client)
        return;

    DeviceListFilter filter;
    if (const auto user_id = parse_int64_param(req->getParameter("userId")))
        filter.user_id = *user_id;
    // roomId <= 0 means "all rooms" (agent device tools use 0 the same way).
    if (const auto room_id = parse_int64_param(req->getParameter("roomId")))
    {
        if (*room_id > 0)
            filter.room_id = *room_id;
    }
    if (!req->getParameter("class").empty())
        filter.device_class = req->getParameter("class");
    if (const auto connected = parse_bool_param(req->getParameter("connected")))
        filter.connected = *connected;
    if (const auto enabled = parse_bool_param(req->getParameter("enabled")))
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
        respond_device_error(callback, code, "장치 목록을 조회할 수 없습니다.");
    else
    {
        auto resp = drogon::HttpResponse::newHttpJsonResponse(body);
        if (runtime_id)
            attachDemoRuntimeCookieIfNeeded(req, resp, *runtime_id);
        callback(resp);
    }
}

void InternalController::getDevice(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string deviceId)
{
    const auto client = require_db(callback);
    if (!client)
        return;

    std::optional<int64_t> user_id;
    if (const auto parsed = parse_int64_param(req->getParameter("userId")))
        user_id = *parsed;

    DevicesInternalStore store(client);
    std::string code;
    const auto runtime_id = demoVirtualDevicesEnabled()
        ? std::optional<std::string>(resolveDemoRuntimeId(req, nullptr))
        : std::nullopt;
    const auto body = store.getDevice(deviceId, user_id, code, runtime_id);
    if (!code.empty())
        respond_device_error(callback, code, "기기를 찾을 수 없습니다.");
    else
    {
        auto resp = drogon::HttpResponse::newHttpJsonResponse(body);
        if (runtime_id)
            attachDemoRuntimeCookieIfNeeded(req, resp, *runtime_id);
        callback(resp);
    }
}

void InternalController::listDeviceClasses(const HttpRequestPtr& /*req*/, HttpResponseCallback&& callback)
{
    callback(drogon::HttpResponse::newHttpJsonResponse(DeviceClassRegistry::list_classes()));
}

void InternalController::getDeviceState(const HttpRequestPtr& req, HttpResponseCallback&& callback,
    std::string deviceId)
{
    const auto client = require_db(callback);
    if (!client)
        return;

    std::optional<int64_t> user_id;
    if (const auto parsed = parse_int64_param(req->getParameter("userId")))
        user_id = *parsed;

    DevicesInternalStore store(client);
    std::string code;
    const auto runtime_id = demoVirtualDevicesEnabled()
        ? std::optional<std::string>(resolveDemoRuntimeId(req, nullptr))
        : std::nullopt;
    const auto body = store.getState(deviceId, user_id, code, runtime_id);
    if (!code.empty())
        respond_device_error(callback, code, "상태 조회에 실패했습니다.");
    else
    {
        auto resp = drogon::HttpResponse::newHttpJsonResponse(body);
        if (runtime_id)
            attachDemoRuntimeCookieIfNeeded(req, resp, *runtime_id);
        callback(resp);
    }
}

void InternalController::queryDevice(const HttpRequestPtr& req, HttpResponseCallback&& callback,
    std::string deviceId,
    std::string queryName)
{
    const auto client = require_db(callback);
    if (!client)
        return;

    Json::Value body(Json::objectValue);
    const auto json = req->getJsonObject();
    if (json && json->isObject())
        body = *json;
    enrich_demo_runtime_body(req, body);

    DevicesInternalStore store(client);
    std::string code;
    const auto response = store.queryDevice(deviceId, queryName, body, code);
    if (!code.empty())
        respond_device_error(callback, code, "조회에 실패했습니다.");
    else
    {
        auto resp = drogon::HttpResponse::newHttpJsonResponse(response);
        attach_demo_runtime_cookie(req, resp, body);
        callback(resp);
    }
}

void InternalController::invokeDeviceAction(const HttpRequestPtr& req, HttpResponseCallback&& callback,
    std::string deviceId,
    std::string actionName)
{
    const auto client = require_db(callback);
    if (!client)
        return;

    Json::Value body(Json::objectValue);
    const auto json = req->getJsonObject();
    if (json && json->isObject())
        body = *json;
    enrich_demo_runtime_body(req, body);

    DevicesInternalStore store(client);
    std::string code;
    const auto response = store.invokeAction(deviceId, actionName, body, code);
    if (!code.empty())
        respond_device_error(callback, code, "제어에 실패했습니다.");
    else
    {
        auto resp = drogon::HttpResponse::newHttpJsonResponse(response);
        attach_demo_runtime_cookie(req, resp, body);
        callback(resp);
    }
}

void InternalController::getPtzCapabilities(const HttpRequestPtr& /*req*/, HttpResponseCallback&& callback,
    std::string deviceId)
{
    const auto client = require_db(callback);
    if (!client)
        return;

    const auto manifest_id = resolve_device_path_id(client, deviceId);
    if (!manifest_id)
    {
        respond_device_error(callback, "NOT_FOUND", "기기를 찾을 수 없습니다.");
        return;
    }

    auto& state = AppState::get();
    v1::IotStore store(state.deviceManager);
    if (!require_iot_devices(callback, store))
        return;

    std::string code;
    const auto body = store.getPtzCapabilities(*manifest_id, code);
    if (!code.empty())
        v1::respondError(callback, map_camera_error_status(code), code, "PTZ 기능을 조회할 수 없습니다.");
    else
        callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

void InternalController::movePtz(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string deviceId)
{
    const auto client = require_db(callback);
    if (!client)
        return;

    const auto manifest_id = resolve_device_path_id(client, deviceId);
    if (!manifest_id)
    {
        respond_device_error(callback, "NOT_FOUND", "기기를 찾을 수 없습니다.");
        return;
    }

    auto& state = AppState::get();
    v1::IotStore store(state.deviceManager);
    if (!require_iot_devices(callback, store))
        return;

    const auto json = req->getJsonObject();
    Json::Value vector(Json::objectValue);
    if (json && json->isObject())
        vector = *json;

    std::string code;
    if (!store.moveCameraPtz(*manifest_id, vector, code))
        v1::respondError(callback, map_camera_error_status(code), code, "PTZ 이동에 실패했습니다.");
    else
    {
        Json::Value body;
        body["ok"] = true;
        callback(drogon::HttpResponse::newHttpJsonResponse(body));
    }
}

void InternalController::stopPtz(const HttpRequestPtr& /*req*/, HttpResponseCallback&& callback, std::string deviceId)
{
    const auto client = require_db(callback);
    if (!client)
        return;

    const auto manifest_id = resolve_device_path_id(client, deviceId);
    if (!manifest_id)
    {
        respond_device_error(callback, "NOT_FOUND", "기기를 찾을 수 없습니다.");
        return;
    }

    auto& state = AppState::get();
    v1::IotStore store(state.deviceManager);
    if (!require_iot_devices(callback, store))
        return;

    std::string code;
    if (!store.stopCameraPtz(*manifest_id, code))
        v1::respondError(callback, map_camera_error_status(code), code, "PTZ 정지에 실패했습니다.");
    else
    {
        Json::Value body;
        body["ok"] = true;
        callback(drogon::HttpResponse::newHttpJsonResponse(body));
    }
}

void InternalController::zoomPtz(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string deviceId)
{
    const auto client = require_db(callback);
    if (!client)
        return;

    const auto manifest_id = resolve_device_path_id(client, deviceId);
    if (!manifest_id)
    {
        respond_device_error(callback, "NOT_FOUND", "기기를 찾을 수 없습니다.");
        return;
    }

    auto& state = AppState::get();
    v1::IotStore store(state.deviceManager);
    if (!require_iot_devices(callback, store))
        return;

    const auto json = req->getJsonObject();
    const int delta = json && json->isMember("delta") ? (*json)["delta"].asInt() : 0;

    std::string code;
    const auto body = store.zoomCameraPtz(*manifest_id, delta, code);
    if (!code.empty())
        v1::respondError(callback, map_camera_error_status(code), code, "줌 조정에 실패했습니다.");
    else
        callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

void InternalController::getCameraStream(const HttpRequestPtr& /*req*/, HttpResponseCallback&& callback,
    std::string deviceId)
{
    const auto client = require_db(callback);
    if (!client)
        return;

    const auto manifest_id = resolve_device_path_id(client, deviceId);
    if (!manifest_id)
    {
        respond_device_error(callback, "NOT_FOUND", "기기를 찾을 수 없습니다.");
        return;
    }

    auto& state = AppState::get();
    v1::IotStore store(state.deviceManager);
    if (!require_iot_devices(callback, store))
        return;

    std::string code;
    const auto body = store.getCameraStream(*manifest_id, code);
    if (!code.empty())
        v1::respondError(callback, map_camera_error_status(code), code, "스트림 정보를 가져올 수 없습니다.");
    else
        callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

void InternalController::setCameraStream(const HttpRequestPtr& req, HttpResponseCallback&& callback,
    std::string deviceId)
{
    const auto client = require_db(callback);
    if (!client)
        return;

    const auto manifest_id = resolve_device_path_id(client, deviceId);
    if (!manifest_id)
    {
        respond_device_error(callback, "NOT_FOUND", "기기를 찾을 수 없습니다.");
        return;
    }

    auto& state = AppState::get();
    v1::IotStore store(state.deviceManager);
    if (!require_iot_devices(callback, store))
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
        v1::respondError(callback, map_camera_error_status(code), code, "스트림을 시작할 수 없습니다.");
    else
        callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

void InternalController::captureSnapshot(const HttpRequestPtr& /*req*/, HttpResponseCallback&& callback,
    std::string deviceId)
{
    const auto client = require_db(callback);
    if (!client)
        return;

    const auto manifest_id = resolve_device_path_id(client, deviceId);
    if (!manifest_id)
    {
        respond_device_error(callback, "NOT_FOUND", "기기를 찾을 수 없습니다.");
        return;
    }

    auto& state = AppState::get();
    v1::IotStore store(state.deviceManager);
    if (!require_iot_devices(callback, store))
        return;

    std::thread([manifest_id = *manifest_id, callback = std::move(callback), &state]() mutable
    {
        v1::IotStore worker(state.deviceManager);
        std::vector<uint8_t> jpeg;
        std::string occurred_at;
        std::string code;
        if (!worker.captureCameraSnapshot(manifest_id, jpeg, occurred_at, code))
        {
            v1::respondError(callback, map_camera_error_status(code), code, "스냅샷 캡처에 실패했습니다.");
            return;
        }

        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setContentTypeCode(drogon::ContentType::CT_IMAGE_JPG);
        resp->setBody(std::string(jpeg.begin(), jpeg.end()));
        resp->addHeader("X-Snapshot-At", occurred_at);
        callback(resp);
    }).detach();
}

void InternalController::sendTts(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string deviceId)
{
    const auto client = require_db(callback);
    if (!client)
        return;

    const auto manifest_id = resolve_device_path_id(client, deviceId);
    if (!manifest_id)
    {
        respond_device_error(callback, "NOT_FOUND", "기기를 찾을 수 없습니다.");
        return;
    }

    auto& state = AppState::get();
    v1::IotStore store(state.deviceManager);
    if (!require_iot_devices(callback, store))
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

void InternalController::listRules(const HttpRequestPtr& req, HttpResponseCallback&& callback)
{
    RuleListFilter filter;
    if (!req->getParameter("deviceId").empty())
        filter.device_id = req->getParameter("deviceId");
    if (const auto enabled = parse_bool_param(req->getParameter("enabled")))
        filter.enabled = *enabled;
    if (const auto has_schedule = parse_bool_param(req->getParameter("hasSchedule")))
        filter.has_schedule = *has_schedule;
    if (const auto has_trigger = parse_bool_param(req->getParameter("hasTrigger")))
        filter.has_trigger = *has_trigger;

    RulesInternalStore store;
    std::string code;
    if (demoVirtualDevicesEnabled())
    {
        const auto client = AppState::get().db();
        const auto runtime_id = resolveDemoRuntimeId(req, nullptr);
        ensureDemoSessionSeeded(runtime_id, client);
        Json::Value items(Json::arrayValue);
        for (const auto& raw : demoListRules(runtime_id, 0))
        {
            if (!raw.isObject())
                continue;

            Json::Value item = raw;
            const bool has_schedule = item.isMember("schedule") && item["schedule"].isObject()
                && !item["schedule"].isNull() && item["schedule"].isMember("repeat");
            const bool has_trigger = item.isMember("trigger") && item["trigger"].isObject()
                && !item["trigger"].isNull() && !item["trigger"].empty();

            if (filter.device_id)
            {
                const auto action_id = item.isMember("action") && item["action"].isObject()
                    ? item["action"].get("deviceId", "").asString()
                    : std::string{};
                const auto trigger_id = has_trigger
                    ? item["trigger"].get("deviceId", "").asString()
                    : std::string{};
                if (action_id != *filter.device_id && trigger_id != *filter.device_id)
                    continue;
            }
            if (filter.enabled && item.get("enabled", true).asBool() != *filter.enabled)
                continue;
            if (filter.has_schedule && has_schedule != *filter.has_schedule)
                continue;
            if (filter.has_trigger && has_trigger != *filter.has_trigger)
                continue;

            items.append(item);
        }

        Json::Value body;
        body["items"] = items;
        body["count"] = static_cast<Json::UInt>(items.size());
        auto resp = drogon::HttpResponse::newHttpJsonResponse(body);
        attachDemoRuntimeCookieIfNeeded(req, resp, runtime_id);
        callback(resp);
        return;
    }

    const auto body = store.listRules(filter, code);
    if (!code.empty())
        respond_device_error(callback, code, "룰 목록을 조회할 수 없습니다.");
    else
        callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

void InternalController::getRule(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string ruleId)
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
        respond_device_error(callback, "NOT_FOUND", "룰을 찾을 수 없습니다.");
        return;
    }

    RulesInternalStore store;
    std::string code;
    const auto body = store.getRule(ruleId, code);
    if (!code.empty())
        respond_device_error(callback, code, "룰을 찾을 수 없습니다.");
    else
        callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

void InternalController::createRule(const HttpRequestPtr& req, HttpResponseCallback&& callback)
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
        enrich_demo_runtime_body(req, body);
        const auto runtime_id = body["demoRuntimeId"].asString();
        ensureDemoSessionSeeded(runtime_id, AppState::get().db());
        const auto created = demoCreateRule(runtime_id, body, code);
        if (created.isNull())
            respond_device_error(callback, code.empty() ? "INVALID_REQUEST" : code, "룰을 생성할 수 없습니다.");
        else
        {
            auto resp = drogon::HttpResponse::newHttpJsonResponse(created);
            resp->setStatusCode(drogon::k201Created);
            attach_demo_runtime_cookie(req, resp, body);
            callback(resp);
        }
        return;
    }

    const auto body = store.createRule(*json, code);
    if (!code.empty())
        respond_device_error(callback, code, "룰을 생성할 수 없습니다.");
    else
    {
        auto resp = drogon::HttpResponse::newHttpJsonResponse(body);
        resp->setStatusCode(drogon::k201Created);
        callback(resp);
    }
}

void InternalController::updateRule(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string ruleId)
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
        enrich_demo_runtime_body(req, body);
        const auto runtime_id = body["demoRuntimeId"].asString();
        ensureDemoSessionSeeded(runtime_id, AppState::get().db());
        std::string code;
        const auto updated = demoUpdateRule(runtime_id, ruleId, body, code);
        if (!code.empty())
            respond_device_error(callback, code, "룰을 수정할 수 없습니다.");
        else
        {
            auto resp = drogon::HttpResponse::newHttpJsonResponse(updated);
            attach_demo_runtime_cookie(req, resp, body);
            callback(resp);
        }
        return;
    }

    RulesInternalStore store;
    std::string code;
    const auto body = store.updateRule(ruleId, *json, code);
    if (!code.empty())
        respond_device_error(callback, code, "룰을 수정할 수 없습니다.");
    else
        callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

void InternalController::deleteRule(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string ruleId)
{
    if (demoVirtualDevicesEnabled())
    {
        const auto runtime_id = resolveDemoRuntimeId(req, nullptr);
        ensureDemoSessionSeeded(runtime_id, AppState::get().db());
        if (!demoDeleteRule(runtime_id, ruleId))
        {
            respond_device_error(callback, "NOT_FOUND", "룰을 삭제할 수 없습니다.");
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
        respond_device_error(callback, code, "룰을 삭제할 수 없습니다.");
    else
        callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

void InternalController::setRuleEnabled(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string ruleId)
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
        enrich_demo_runtime_body(req, body);
        const auto runtime_id = body["demoRuntimeId"].asString();
        ensureDemoSessionSeeded(runtime_id, AppState::get().db());
        std::string code;
        const auto updated = demoUpdateRule(runtime_id, ruleId, body, code);
        if (!code.empty())
            respond_device_error(callback, code, "룰 상태를 변경할 수 없습니다.");
        else
        {
            auto resp = drogon::HttpResponse::newHttpJsonResponse(updated);
            attach_demo_runtime_cookie(req, resp, body);
            callback(resp);
        }
        return;
    }

    RulesInternalStore store;
    std::string code;
    const auto body = store.setRuleEnabled(ruleId, (*json)["enabled"].asBool(), code);
    if (!code.empty())
        respond_device_error(callback, code, "룰 상태를 변경할 수 없습니다.");
    else
        callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

void InternalController::executeRule(const HttpRequestPtr& /*req*/, HttpResponseCallback&& callback, std::string ruleId)
{
    RulesInternalStore store;
    std::string code;
    const auto body = store.executeRule(ruleId, code);
    if (!code.empty())
        respond_device_error(callback, code, "룰 실행에 실패했습니다.");
    else
        callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

void InternalController::listIrCommands(const HttpRequestPtr& req, HttpResponseCallback&& callback)
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

void InternalController::getIrCommand(const HttpRequestPtr& /*req*/, HttpResponseCallback&& callback,
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

void InternalController::listEvents(const HttpRequestPtr& req, HttpResponseCallback&& callback)
{
    const auto client = require_db(callback);
    if (!client)
        return;

    EventsListFilter filter;
    if (!req->getParameter("types").empty())
        filter.types = split_csv(req->getParameter("types"));
    if (!req->getParameter("deviceId").empty())
        filter.device_id = req->getParameter("deviceId");
    if (!req->getParameter("from").empty())
        filter.from = req->getParameter("from");
    if (!req->getParameter("to").empty())
        filter.to = req->getParameter("to");
    if (const auto limit = parse_int64_param(req->getParameter("limit")))
        filter.limit = static_cast<int>(*limit);

    DevicesInternalStore store(client);
    std::string code;
    const auto body = store.listEvents(filter, code);
    if (!code.empty())
        respond_device_error(callback, code, "이벤트를 조회할 수 없습니다.");
    else
        callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

void InternalController::toolListDevices(const HttpRequestPtr& req, HttpResponseCallback&& callback)
{
    const auto client = require_db(callback);
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
        respond_device_error(callback, code, "장치 목록을 조회할 수 없습니다.");
    else
        callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

void InternalController::toolControlDevice(const HttpRequestPtr& req, HttpResponseCallback&& callback)
{
    const auto client = require_db(callback);
    if (!client)
        return;

    const auto json = req->getJsonObject();
    if (!json)
    {
        v1::respondError(callback, 400, "INVALID_BODY", "요청 본문이 필요합니다.");
        return;
    }

    Json::Value body = *json;
    enrich_demo_runtime_body(req, body);

    DevicesInternalStore store(client);
    std::string code;
    const auto response = store.toolControlDevice(body, code);
    if (!code.empty())
        respond_device_error(callback, code, "제어에 실패했습니다.");
    else
    {
        auto resp = drogon::HttpResponse::newHttpJsonResponse(response);
        attach_demo_runtime_cookie(req, resp, body);
        callback(resp);
    }
}

void InternalController::toolQueryDevice(const HttpRequestPtr& req, HttpResponseCallback&& callback)
{
    const auto client = require_db(callback);
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
        respond_device_error(callback, code, "조회에 실패했습니다.");
    else
        callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

void InternalController::toolSchedule(const HttpRequestPtr& req, HttpResponseCallback&& callback)
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
        respond_device_error(callback, code, "예약을 생성할 수 없습니다.");
    else
    {
        auto resp = drogon::HttpResponse::newHttpJsonResponse(body);
        resp->setStatusCode(drogon::k201Created);
        callback(resp);
    }
}

void InternalController::toolScheduleList(const HttpRequestPtr& req, HttpResponseCallback&& callback)
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
        respond_device_error(callback, code, "예약 목록을 조회할 수 없습니다.");
    else
        callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

void InternalController::toolScheduleCancel(const HttpRequestPtr& req, HttpResponseCallback&& callback)
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
        respond_device_error(callback, code, "예약을 취소할 수 없습니다.");
    else
        callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

void InternalController::listAlarms(const HttpRequestPtr& req, HttpResponseCallback&& callback)
{
    const auto client = require_db(callback);
    if (!client)
        return;

    const auto user_id = parse_int64_param(req->getParameter("userId"));
    if (!user_id)
    {
        v1::respondError(callback, 400, "INVALID_REQUEST", "userId가 필요합니다.", "userId");
        return;
    }

    AlarmListFilter filter;
    filter.user_id = *user_id;
    if (const auto enabled = parse_bool_param(req->getParameter("enabled")))
        filter.enabled = *enabled;

    std::optional<bool> enabled_filter = filter.enabled;
    const auto runtime_id = demoVirtualDevicesEnabled()
        ? resolveDemoRuntimeId(req, nullptr)
        : std::string();
    auto resp = drogon::HttpResponse::newHttpJsonResponse(
        AppState::get().runtime().alarms().list(*user_id, enabled_filter, runtime_id, client));
    if (!runtime_id.empty())
        attachDemoRuntimeCookieIfNeeded(req, resp, runtime_id);
    callback(resp);
}

void InternalController::createAlarm(const HttpRequestPtr& req, HttpResponseCallback&& callback)
{
    const auto client = require_db(callback);
    if (!client)
        return;

    const auto json = req->getJsonObject();
    if (!json)
    {
        v1::respondError(callback, 400, "INVALID_BODY", "요청 본문이 필요합니다.");
        return;
    }

    std::string error;
    std::string field;
    Json::Value body = *json;
    if (demoVirtualDevicesEnabled())
        enrich_demo_runtime_body(req, body);
    const auto runtime_id = demoVirtualDevicesEnabled()
        ? body.get("demoRuntimeId", "").asString()
        : std::string();
    const auto created = AppState::get().runtime().alarms().create(body, runtime_id, client, error, field);
    if (created.isNull())
        v1::respondError(callback, 400, "VALIDATION_ERROR", error, field);
    else
    {
        auto resp = drogon::HttpResponse::newHttpJsonResponse(created);
        resp->setStatusCode(drogon::k201Created);
        if (!runtime_id.empty())
            attach_demo_runtime_cookie(req, resp, body);
        callback(resp);
    }
}

void InternalController::updateAlarm(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string id)
{
    const auto client = require_db(callback);
    if (!client)
        return;

    const auto user_id = parse_int64_param(req->getParameter("userId"));
    const auto alarm_id = parse_int64_param(id);
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
    const auto runtime_id = demoVirtualDevicesEnabled()
        ? resolveDemoRuntimeId(req, json.get())
        : std::string();
    const auto updated = AppState::get().runtime().alarms().update(
        *user_id, *alarm_id, *json, runtime_id, client, error, field);
    if (updated.isNull())
    {
        const int status = error.find("찾을") != std::string::npos ? 404 : 400;
        v1::respondError(callback, status, status == 404 ? "NOT_FOUND" : "VALIDATION_ERROR", error, field);
        return;
    }
    auto resp = drogon::HttpResponse::newHttpJsonResponse(updated);
    if (!runtime_id.empty())
        attachDemoRuntimeCookieIfNeeded(req, resp, runtime_id);
    callback(resp);
}

void InternalController::deleteAlarm(const HttpRequestPtr& req, HttpResponseCallback&& callback, std::string id)
{
    const auto client = require_db(callback);
    if (!client)
        return;

    const auto user_id = parse_int64_param(req->getParameter("userId"));
    const auto alarm_id = parse_int64_param(id);
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

    std::string error;
    const auto runtime_id = demoVirtualDevicesEnabled()
        ? resolveDemoRuntimeId(req, nullptr)
        : std::string();
    const auto removed = AppState::get().runtime().alarms().remove(
        *user_id, *alarm_id, runtime_id, client, error);
    if (removed.isNull())
    {
        v1::respondError(callback, 404, "NOT_FOUND", error);
        return;
    }
    auto resp = drogon::HttpResponse::newHttpJsonResponse(removed);
    if (!runtime_id.empty())
        attachDemoRuntimeCookieIfNeeded(req, resp, runtime_id);
    callback(resp);
}

void InternalController::listScheduleTasks(const HttpRequestPtr& req, HttpResponseCallback&& callback)
{
    const auto client = require_db(callback);
    if (!client)
        return;

    const auto user_id = parse_int64_param(req->getParameter("userId"));
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
    if (const auto done = parse_bool_param(req->getParameter("done")))
        filter.done = *done;

    const auto runtime_id = demoVirtualDevicesEnabled()
        ? resolveDemoRuntimeId(req, nullptr)
        : std::string();
    auto resp = drogon::HttpResponse::newHttpJsonResponse(
        AppState::get().runtime().scheduleTasks().list(filter, runtime_id, client));
    if (!runtime_id.empty())
        attachDemoRuntimeCookieIfNeeded(req, resp, runtime_id);
    callback(resp);
}

void InternalController::createScheduleTask(const HttpRequestPtr& req, HttpResponseCallback&& callback)
{
    const auto client = require_db(callback);
    if (!client)
        return;

    const auto json = req->getJsonObject();
    if (!json)
    {
        v1::respondError(callback, 400, "INVALID_BODY", "요청 본문이 필요합니다.");
        return;
    }

    std::string error;
    std::string field;
    Json::Value body = *json;
    if (demoVirtualDevicesEnabled())
        enrich_demo_runtime_body(req, body);
    const auto runtime_id = demoVirtualDevicesEnabled()
        ? body.get("demoRuntimeId", "").asString()
        : std::string();
    const auto created = AppState::get().runtime().scheduleTasks().create(
        body, runtime_id, client, error, field);
    if (!created)
    {
        v1::respondError(callback, 400, "INVALID_REQUEST", error, field);
        return;
    }
    auto resp = drogon::HttpResponse::newHttpJsonResponse(*created);
    resp->setStatusCode(drogon::k201Created);
    if (!runtime_id.empty())
        attach_demo_runtime_cookie(req, resp, body);
    callback(resp);
}

void InternalController::updateScheduleTask(const HttpRequestPtr& req, HttpResponseCallback&& callback,
    std::string taskId)
{
    const auto client = require_db(callback);
    if (!client)
        return;

    const auto user_id = parse_int64_param(req->getParameter("userId"));
    const auto id = parse_int64_param(taskId);
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
    const auto runtime_id = demoVirtualDevicesEnabled()
        ? resolveDemoRuntimeId(req, json.get())
        : std::string();
    const auto updated = AppState::get().runtime().scheduleTasks().update(
        *user_id, *id, *json, runtime_id, client, error, field);
    if (!updated)
    {
        const int status = error.find("찾을") != std::string::npos ? 404 : 400;
        const std::string code = status == 404 ? "NOT_FOUND" : "INVALID_REQUEST";
        v1::respondError(callback, status, code, error, field);
        return;
    }
    auto resp = drogon::HttpResponse::newHttpJsonResponse(*updated);
    if (!runtime_id.empty())
        attachDemoRuntimeCookieIfNeeded(req, resp, runtime_id);
    callback(resp);
}

void InternalController::deleteScheduleTask(const HttpRequestPtr& req, HttpResponseCallback&& callback,
    std::string taskId)
{
    const auto client = require_db(callback);
    if (!client)
        return;

    const auto user_id = parse_int64_param(req->getParameter("userId"));
    const auto id = parse_int64_param(taskId);
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

    std::string error;
    const auto runtime_id = demoVirtualDevicesEnabled()
        ? resolveDemoRuntimeId(req, nullptr)
        : std::string();
    const auto removed = AppState::get().runtime().scheduleTasks().remove(
        *user_id, *id, runtime_id, client, error);
    if (!removed)
    {
        v1::respondError(callback, 404, "NOT_FOUND", error);
        return;
    }
    auto resp = drogon::HttpResponse::newHttpJsonResponse(*removed);
    if (!runtime_id.empty())
        attachDemoRuntimeCookieIfNeeded(req, resp, runtime_id);
    callback(resp);
}

} // namespace internal
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
