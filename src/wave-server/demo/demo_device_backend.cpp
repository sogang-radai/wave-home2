#include "demo_device_backend.h"

#include <unordered_set>

#include "../app/app_state.h"
#include "../device/device_wire_id.hpp"
#include "../web/http/internal/device_class_registry.h"
#include "demo_device_simulator.h"
#include "demo_power_meter.h"
#include "demo_runtime_id.h"
#include "demo_session_registry.h"

WAVE_NAMESPACE_BEGIN

namespace
{
    std::string panel_for_class(const std::string& device_class)
    {
        if (device_class == "srs_r4sn")
            return "radar";
        if (device_class == "wave_station")
            return "wave_station";
        if (device_class == "reolink_e1_pro" || device_class == "droid_cam")
            return "camera";
        if (device_class == "tuya_ep2h")
            return "plug";
        if (device_class == "samsung_g7" || device_class == "tizen_tv")
            return "tv";
        if (device_class.rfind("philips_wiz_e29", 0) == 0)
            return "light";
        return "";
    }

    double rated_power_for_device(const std::string& device_id)
    {
        if (device_id == "0000000000000006")
            return 20.0;
        if (device_id == "0000000000000007")
            return 100.0;
        if (device_id == "0000000000000008")
            return 600.0;
        if (device_id == "0000000000000009")
            return 2400.0;
        return 0.0;
    }
}

bool demoVirtualDevicesEnabled()
{
    const auto& state = AppState::get();
    return state.demo_mode && state.no_devices;
}

std::string resolveDemoRuntimeId(const drogon::HttpRequestPtr& req, const Json::Value* body)
{
    if (body && body->isMember("demoRuntimeId") && (*body)["demoRuntimeId"].isString())
    {
        const auto id = (*body)["demoRuntimeId"].asString();
        if (!id.empty())
            return id;
    }
    if (const auto from_header = demoRuntimeIdFromHeader(req))
        return *from_header;
    if (const auto from_cookie = demoRuntimeIdFromCookie(req))
        return *from_cookie;
    return generateDemoRuntimeId();
}

DemoDeviceBackend::DemoDeviceBackend(drogon::orm::DbClientPtr client) :
    m_client(std::move(client))
{
}

Json::Value DemoDeviceBackend::deviceRowToJson(const drogon::orm::Row& row, const Json::Value& runtime_state) const
{
    const int64_t db_id = row["id"].as<int64_t>();
    const std::string device_class = row["class"].as<std::string>();
    const std::string wire_id = dev::wireIdForDbRow(db_id, row["name"].as<std::string>());

    Json::Value item;
    item["id"] = wire_id;
    item["name"] = row["name"].as<std::string>();
    item["description"] = row["description"].as<std::string>();
    item["class"] = device_class;
    item["classLabel"] = web::internal::DeviceClassRegistry::labelForClass(device_class);
    item["panel"] = panel_for_class(device_class);
    item["sleepAnalysis"] =
        !row["sleep_analysis"].isNull() &&
        row["sleep_analysis"].as<int>() != 0;
    item["enabled"] = row["enabled"].as<int>() != 0;
    item["connected"] = device_class != "droid_cam";
    item["stateSummary"] = "데모";

    if (!row["room_id"].isNull())
    {
        Json::Value room;
        room["id"] = static_cast<Json::Int64>(row["room_id"].as<int64_t>());
        room["name"] = row["room_name"].as<std::string>();
        item["room"] = room;
    }

    if (runtime_state.isObject())
    {
        if (device_class == "tuya_ep2h")
            item["stateSummary"] = runtime_state.get("switch", false).asBool() ? "켜짐" : "꺼짐";
        else if (device_class == "philips_wiz_e29_color" || device_class == "philips_wiz_e29_white")
            item["stateSummary"] = runtime_state.get("on", false).asBool() ? "켜짐" : "꺼짐";
        else if (device_class == "samsung_g7" || device_class == "tizen_tv")
            item["stateSummary"] = runtime_state.get("on", false).asBool() ? "켜짐" : "꺼짐";
    }

    return item;
}

Json::Value DemoDeviceBackend::stateForDevice(
    const std::string& runtime_id,
    const std::string& device_id,
    const std::string& device_class) const
{
    auto& registry = DemoSessionRegistry::instance();
    auto& session = registry.touch(runtime_id);
    const auto it = session.device_state.find(device_id);
    if (it != session.device_state.end())
        return it->second;
    auto state = demoSeedStateForClass(device_class);
    if (device_class == "tuya_ep2h")
    {
        const double rated_power = rated_power_for_device(device_id);
        const bool switch_on = device_id != "0000000000000009";
        state["switch"] = switch_on;
        state["ratedPower"] = rated_power;
        state["voltage"] = 235.0;
        const auto reading = DemoPowerMeter::instance().samplePlug(
            runtime_id, device_id, switch_on, rated_power, 235.0);
        state["power"] = reading.power_w;
        state["current"] = reading.current_ma;
        state["voltage"] = reading.voltage_v;
    }
    // Persist the seed so later reads/actions share the same session entry.
    session.device_state[device_id] = state;
    return state;
}

void DemoDeviceBackend::saveState(
    const std::string& runtime_id,
    const std::string& device_id,
    const Json::Value& state) const
{
    auto& session = DemoSessionRegistry::instance().touch(runtime_id);
    session.device_state[device_id] = state;
}

Json::Value DemoDeviceBackend::listDevices(const std::string& runtime_id, std::string& code) const
{
    if (!m_client)
    {
        code = "DEVICES_UNAVAILABLE";
        return Json::Value();
    }

    auto rows = m_client->execSqlSync(
        R"SQL(
SELECT d.id, d.name, d.description, d.class, d.enabled,
       COALESCE(json_extract(d.settings_json, '$.sleep'), 0) AS sleep_analysis,
       r.id AS room_id, r.name AS room_name
FROM device d
LEFT JOIN device_room_map drm ON drm.device_id = d.id
LEFT JOIN room r ON r.id = drm.room_id
WHERE d.archived = 0
ORDER BY d.id
)SQL");

    Json::Value items(Json::arrayValue);
    for (const auto& row : rows)
    {
        const std::string wire_id = dev::wireIdForDbRow(row["id"].as<int64_t>(), row["name"].as<std::string>());
        const auto state = stateForDevice(runtime_id, wire_id, row["class"].as<std::string>());
        items.append(deviceRowToJson(row, state));
    }

    Json::Value body;
    body["items"] = items;
    body["count"] = static_cast<Json::UInt>(items.size());
    return body;
}

Json::Value DemoDeviceBackend::getState(
    const std::string& runtime_id,
    const std::string& device_id,
    std::string& code) const
{
    if (!m_client)
    {
        code = "DEVICES_UNAVAILABLE";
        return Json::Value();
    }

    const auto db_id = dev::dbIdForWireId(m_client, device_id);
    if (!db_id)
    {
        code = "NOT_FOUND";
        return Json::Value();
    }

    auto rows = m_client->execSqlSync(
        "SELECT class FROM device WHERE id = ? AND archived = 0 LIMIT 1",
        *db_id);
    if (rows.empty())
    {
        code = "NOT_FOUND";
        return Json::Value();
    }

    const auto device_class = rows[0]["class"].as<std::string>();
    auto state = stateForDevice(runtime_id, device_id, device_class);
    if (device_class == "tuya_ep2h")
    {
        const auto reading = DemoPowerMeter::instance().samplePlug(
            runtime_id,
            device_id,
            state.get("switch", false).asBool(),
            state.get("ratedPower", rated_power_for_device(device_id)).asDouble(),
            state.get("voltage", 235.0).asDouble());
        state["power"] = reading.power_w;
        state["current"] = reading.current_ma;
        state["voltage"] = reading.voltage_v;
        saveState(runtime_id, device_id, state);
    }

    Json::Value body;
    body["deviceId"] = device_id;
    body["connected"] = device_class != "droid_cam";
    body["state"] = state;
    return body;
}

Json::Value DemoDeviceBackend::queryDevice(
    const std::string& runtime_id,
    const std::string& device_id,
    const std::string& query_name,
    std::string& code) const
{
    const auto state_body = getState(runtime_id, device_id, code);
    if (!code.empty())
        return Json::Value();

    Json::Value state = state_body["state"];
    if (state.isObject() && state.isMember("ratedPower"))
    {
        const auto reading = DemoPowerMeter::instance().samplePlug(
            runtime_id,
            device_id,
            state.get("switch", false).asBool(),
            state.get("ratedPower", 0.0).asDouble(),
            state.get("voltage", 235.0).asDouble());
        state["power"] = reading.power_w;
        state["current"] = reading.current_ma;
        state["voltage"] = reading.voltage_v;
        saveState(runtime_id, device_id, state);
    }

    Json::Value response;
    response["deviceId"] = device_id;
    response["query"] = query_name;
    if (query_name == "status" || query_name == "state")
        response["result"] = state;
    else if (state.isObject() && state.isMember(query_name))
        response["result"] = state[query_name];
    else
        response["result"] = state;
    return response;
}

Json::Value DemoDeviceBackend::invokeAction(
    const std::string& runtime_id,
    const std::string& device_id,
    const std::string& action_name,
    const Json::Value& body,
    std::string& code)
{
    if (!m_client)
    {
        code = "DEVICES_UNAVAILABLE";
        return Json::Value();
    }

    const auto db_id = dev::dbIdForWireId(m_client, device_id);
    if (!db_id)
    {
        code = "NOT_FOUND";
        return Json::Value();
    }

    auto rows = m_client->execSqlSync(
        "SELECT class, name FROM device WHERE id = ? AND archived = 0 LIMIT 1",
        *db_id);
    if (rows.empty())
    {
        code = "NOT_FOUND";
        return Json::Value();
    }

    const auto device_class = rows[0]["class"].as<std::string>();
    if (device_class == "droid_cam")
    {
        code = "DEVICE_OFFLINE";
        return Json::Value();
    }

    const auto prev = stateForDevice(runtime_id, device_id, device_class);
    Json::Value params(Json::objectValue);
    if (body.isMember("params") && body["params"].isObject())
        params = body["params"];
    else if (body.isObject())
    {
        static const std::unordered_set<std::string> k_meta = {
            "userId", "reason", "execMode", "triggeredBy", "params", "repeatIntervalMs", "demoRuntimeId"};
        for (const auto& key : body.getMemberNames())
        {
            if (k_meta.count(key) == 0)
                params[key] = body[key];
        }
    }

    auto next = demoApplyAction(device_class, prev, action_name, params);
    if (device_class == "tuya_ep2h")
    {
        const double rated = next.get("ratedPower", rated_power_for_device(device_id)).asDouble();
        const bool switch_on = next.get("switch", false).asBool();
        const double voltage = next.get("voltage", 235.0).asDouble();
        DemoPowerMeter::instance().syncPlug(runtime_id, device_id, switch_on, rated, voltage);
        const auto reading = DemoPowerMeter::instance().samplePlug(
            runtime_id, device_id, switch_on, rated, voltage);
        next["power"] = reading.power_w;
        next["current"] = reading.current_ma;
        next["voltage"] = reading.voltage_v;
        next["ratedPower"] = rated;
    }
    saveState(runtime_id, device_id, next);

    Json::Value response;
    response["ok"] = true;
    response["deviceId"] = device_id;
    response["action"] = action_name;
    response["state"] = next;
    response["deviceName"] = rows[0]["name"].as<std::string>();
    return response;
}

WAVE_NAMESPACE_END
