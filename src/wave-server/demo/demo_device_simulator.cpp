#include "demo_device_simulator.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <optional>
#include <string>

WAVE_NAMESPACE_BEGIN

namespace
{
    std::string toLowerAscii(std::string value)
    {
        for (auto& ch : value)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return value;
    }

    std::string normalizePowerActionName(const std::string& action_name)
    {
        const auto action = toLowerAscii(action_name);
        if (action == "on" || action == "power_on" || action == "turn_on" || action == "switch_on"
            || action == "poweron" || action == "turnon" || action == "switchon"
            || action == "enable" || action == "start" || action == "켜기" || action == "전원켜기"
            || action == "전원_켜기")
            return "on";
        if (action == "off" || action == "power_off" || action == "turn_off" || action == "switch_off"
            || action == "poweroff" || action == "turnoff" || action == "switchoff"
            || action == "disable" || action == "stop" || action == "shutdown" || action == "끄기"
            || action == "전원끄기" || action == "전원_끄기")
            return "off";
        if (action == "toggle" || action == "power_toggle" || action == "switch_toggle"
            || action == "토글" || action == "전환")
            return "toggle";
        // LLMs often confuse the datapoint name "switch" with an action.
        if (action == "switch" || action == "set_switch" || action == "set_power")
            return "switch";
        return action;
    }

    std::optional<bool> extractSwitchTarget(const Json::Value& params)
    {
        if (!params.isObject())
            return std::nullopt;

        auto fromValue = [](const Json::Value& value) -> std::optional<bool> {
            if (value.isBool())
                return value.asBool();
            if (value.isInt() || value.isUInt())
                return value.asInt() != 0;
            if (value.isDouble())
                return value.asDouble() != 0.0;
            if (value.isString())
            {
                const auto text = toLowerAscii(value.asString());
                if (text == "on" || text == "true" || text == "1" || text == "켜기")
                    return true;
                if (text == "off" || text == "false" || text == "0" || text == "끄기")
                    return false;
            }
            return std::nullopt;
        };

        for (const char* key : {"value", "switch", "on", "power", "state", "enabled"})
        {
            if (params.isMember(key))
            {
                if (const auto parsed = fromValue(params[key]))
                    return parsed;
            }
        }
        return std::nullopt;
    }

    int clampInt(int value, int lo, int hi)
    {
        return std::clamp(value, lo, hi);
    }

    int paramInt(const Json::Value& params, const char* key, int fallback)
    {
        if (!params.isObject() || !params.isMember(key))
            return fallback;
        const auto& value = params[key];
        if (value.isInt())
            return value.asInt();
        if (value.isUInt())
            return static_cast<int>(value.asUInt());
        if (value.isDouble())
            return static_cast<int>(value.asDouble());
        if (value.isString())
        {
            try
            {
                return std::stoi(value.asString());
            }
            catch (...)
            {
                return fallback;
            }
        }
        return fallback;
    }

    bool parseHexColor(const std::string& raw, int& r, int& g, int& b)
    {
        std::string hex = raw;
        if (!hex.empty() && hex[0] == '#')
            hex.erase(0, 1);
        if (hex.size() != 6)
            return false;
        for (char c : hex)
        {
            if (!std::isxdigit(static_cast<unsigned char>(c)))
                return false;
        }
        r = static_cast<int>(std::strtol(hex.substr(0, 2).c_str(), nullptr, 16));
        g = static_cast<int>(std::strtol(hex.substr(2, 2).c_str(), nullptr, 16));
        b = static_cast<int>(std::strtol(hex.substr(4, 2).c_str(), nullptr, 16));
        return true;
    }

    bool extractRgb(const Json::Value& params, int& r, int& g, int& b)
    {
        if (!params.isObject())
            return false;

        if (params.isMember("r") || params.isMember("g") || params.isMember("b"))
        {
            r = clampInt(paramInt(params, "r", 255), 0, 255);
            g = clampInt(paramInt(params, "g", 255), 0, 255);
            b = clampInt(paramInt(params, "b", 255), 0, 255);
            return true;
        }

        if (params.isMember("color") && params["color"].isObject())
        {
            const auto& color = params["color"];
            r = clampInt(paramInt(color, "r", 255), 0, 255);
            g = clampInt(paramInt(color, "g", 255), 0, 255);
            b = clampInt(paramInt(color, "b", 255), 0, 255);
            return true;
        }

        if (params.isMember("hex") && params["hex"].isString())
            return parseHexColor(params["hex"].asString(), r, g, b);
        if (params.isMember("color") && params["color"].isString())
            return parseHexColor(params["color"].asString(), r, g, b);

        return false;
    }

    int extractBrightness(const Json::Value& params, int fallback)
    {
        if (params.isMember("value"))
            return clampInt(paramInt(params, "value", fallback), 0, 100);
        if (params.isMember("brightness"))
            return clampInt(paramInt(params, "brightness", fallback), 0, 100);
        if (params.isMember("dimming"))
            return clampInt(paramInt(params, "dimming", fallback), 0, 100);
        return fallback;
    }

    int extractTemperature(const Json::Value& params, int fallback)
    {
        if (params.isMember("value"))
            return clampInt(paramInt(params, "value", fallback), 2200, 6500);
        if (params.isMember("temperature"))
            return clampInt(paramInt(params, "temperature", fallback), 2200, 6500);
        if (params.isMember("temp"))
            return clampInt(paramInt(params, "temp", fallback), 2200, 6500);
        return fallback;
    }
}

Json::Value demoSeedStateForClass(const std::string& device_class)
{
    Json::Value state;
    if (device_class == "tuya_ep2h")
    {
        state["switch"] = true;
        state["voltage"] = 234.6;
        state["current"] = 118.2;
        state["power"] = 27.7;
        state["energy"] = 12.4;
    }
    else if (device_class == "philips_wiz_e29_color")
    {
        state["on"] = true;
        state["brightness"] = 70;
        state["temperature"] = 4000;
        Json::Value color;
        color["r"] = 255;
        color["g"] = 214;
        color["b"] = 170;
        state["color"] = color;
    }
    else if (device_class == "philips_wiz_e29_white")
    {
        state["on"] = true;
        state["brightness"] = 55;
        state["temperature"] = 4200;
    }
    else if (device_class == "samsung_g7" || device_class == "tizen_tv")
    {
        state["on"] = false;
        state["volume"] = 12;
        state["channel"] = 7;
        state["muted"] = false;
        state["app"] = "";
    }
    else if (device_class == "srs_r4sn")
    {
        state["presence"] = true;
        state["posture"] = "sitting";
    }
    else if (device_class == "wave_station")
    {
        state["subscribed"] = false;
        state["micLevel"] = 0.12;
    }
    else if (device_class == "reolink_e1_pro")
    {
        state["streaming"] = false;
        state["ptz"] = Json::Value(Json::objectValue);
    }
    else if (device_class == "droid_cam")
    {
        state["streaming"] = false;
    }
    return state;
}

Json::Value demoApplyAction(
    const std::string& device_class,
    const Json::Value& prev_state,
    const std::string& action_name,
    const Json::Value& params,
    bool& applied)
{
    applied = false;
    Json::Value next = prev_state.isObject() ? prev_state : Json::Value(Json::objectValue);
    // LLMs often invent power_on/power_off; map to the real catalog names.
    const std::string action = normalizePowerActionName(action_name);

    if (device_class == "tuya_ep2h")
    {
        if (action == "on" || action == "toggle")
        {
            next["switch"] = action == "toggle" ? !prev_state.get("switch", false).asBool() : true;
            applied = true;
        }
        else if (action == "off")
        {
            next["switch"] = false;
            applied = true;
        }
        else if (action == "switch")
        {
            if (const auto target = extractSwitchTarget(params))
                next["switch"] = *target;
            else
                next["switch"] = !prev_state.get("switch", false).asBool();
            applied = true;
        }

        if (applied)
        {
            const bool switch_on = next.get("switch", false).asBool();
            const double rated_power = next.get("ratedPower", prev_state.get("power", 0.0)).asDouble();
            next["power"] = switch_on ? rated_power : 0.0;
            next["current"] = switch_on ? rated_power / 235.0 * 1000.0 : 0.0;
        }
    }
    else if (device_class == "philips_wiz_e29_color" || device_class == "philips_wiz_e29_white")
    {
        if (action == "on" || action == "toggle")
        {
            next["on"] = action == "toggle" ? !prev_state.get("on", false).asBool() : true;
            applied = true;
        }
        else if (action == "off")
        {
            next["on"] = false;
            applied = true;
        }
        else if (action == "switch")
        {
            if (const auto target = extractSwitchTarget(params))
                next["on"] = *target;
            else
                next["on"] = !prev_state.get("on", false).asBool();
            applied = true;
        }
        else if (action == "brightness")
        {
            next["brightness"] = extractBrightness(params, prev_state.get("brightness", 50).asInt());
            next["on"] = next["brightness"].asInt() > 0;
            applied = true;
        }
        else if (action == "temperature")
        {
            next["temperature"] = extractTemperature(params, prev_state.get("temperature", 4000).asInt());
            next["on"] = true;
            applied = true;
        }
        else if (action == "color" && device_class == "philips_wiz_e29_color")
        {
            int r = 255;
            int g = 255;
            int b = 255;
            if (extractRgb(params, r, g, b))
            {
                Json::Value color;
                color["r"] = r;
                color["g"] = g;
                color["b"] = b;
                next["color"] = color;
                next["on"] = true;
                applied = true;
            }
        }
    }
    else if (device_class == "samsung_g7" || device_class == "tizen_tv")
    {
        if (action == "on" || action == "toggle")
        {
            next["on"] = action == "toggle" ? !prev_state.get("on", false).asBool() : true;
            applied = true;
        }
        else if (action == "off")
        {
            next["on"] = false;
            applied = true;
        }
        else if (action == "switch")
        {
            if (const auto target = extractSwitchTarget(params))
                next["on"] = *target;
            else
                next["on"] = !prev_state.get("on", false).asBool();
            applied = true;
        }
        else if (action == "volume_up")
        {
            next["volume"] = prev_state.get("volume", 0).asInt() + std::max(1, paramInt(params, "count", 1));
            applied = true;
        }
        else if (action == "volume_down")
        {
            next["volume"] = std::max(0, prev_state.get("volume", 0).asInt() - std::max(1, paramInt(params, "count", 1)));
            applied = true;
        }
        else if (action == "channel_up")
        {
            next["channel"] = prev_state.get("channel", 0).asInt() + std::max(1, paramInt(params, "count", 1));
            applied = true;
        }
        else if (action == "channel_down")
        {
            next["channel"] = std::max(0, prev_state.get("channel", 0).asInt() - std::max(1, paramInt(params, "count", 1)));
            applied = true;
        }
        else if (action == "mute")
        {
            next["muted"] = !prev_state.get("muted", false).asBool();
            applied = true;
        }
        else if (action == "open_app")
        {
            next["app"] = params.get("app", "").asString();
            applied = true;
        }
        else if (action == "input" || action == "input_source" || action == "nav_up" || action == "nav_down"
            || action == "nav_left" || action == "nav_right" || action == "select" || action == "home"
            || action == "back" || action == "play_pause" || action == "send_key")
        {
            // Momentary / catalogued remote keys — ack without lasting state.
            applied = true;
        }
    }
    else if (device_class == "wave_station")
    {
        if (action == "subscribe")
        {
            next["subscribed"] = true;
            applied = true;
        }
        else if (action == "unsubscribe")
        {
            next["subscribed"] = false;
            applied = true;
        }
        else if (action == "send_ir")
        {
            next["lastIrCommandId"] = params.get("commandId", params.get("command", "")).asString();
            applied = true;
        }
    }
    else if (device_class == "reolink_e1_pro")
    {
        if (action == "start_stream")
        {
            next["streaming"] = true;
            applied = true;
        }
        else if (action == "stop_stream")
        {
            next["streaming"] = false;
            applied = true;
        }
    }

    return next;
}

WAVE_NAMESPACE_END
