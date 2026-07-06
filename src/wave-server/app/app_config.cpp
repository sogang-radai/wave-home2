#include "app_config.h"

#include <fstream>

#include "../core/logger.h"

WAVE_NAMESPACE_BEGIN

bool AppConfig::loadFromFile(const std::filesystem::path& path, AppConfig& out)
{
    std::ifstream in(path);
    if (!in.is_open())
    {
        LOG_ERROR("Failed to open config file: {}", path.string());
        return false;
    }

    json root;
    try
    {
        in >> root;
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("Failed to parse config file {}: {}", path.string(), e.what());
        return false;
    }

    if (!root.contains("server") || !root["server"].is_object())
    {
        LOG_ERROR("Config file is missing \"server\" object: {}", path.string());
        return false;
    }

    out.server = root["server"];
    if (root.contains("log_level") && root["log_level"].is_string())
        out.log_level = root["log_level"].get<std::string>();
    if (root.contains("io_threads_num") && root["io_threads_num"].is_number_unsigned())
        out.io_threads_num = root["io_threads_num"].get<uint32_t>();

    auto assignPath = [&](const char* key, std::string& target)
    {
        if (root.contains(key) && root[key].is_string())
            target = root[key].get<std::string>();
    };

    assignPath("setting_path", out.setting_path);
    assignPath("device_list_path", out.device_list_path);
    assignPath("rooms_path", out.rooms_path);
    assignPath("gesture_sets_path", out.gesture_sets_path);
    assignPath("sleep_model_path", out.sleep_model_path);
    assignPath("stt_model_path", out.stt_model_path);
    assignPath("tts_model_path", out.tts_model_path);

    if (root.contains("devices_enabled") && root["devices_enabled"].is_boolean())
        out.devices_enabled = root["devices_enabled"].get<bool>();

    if (root.contains("push") && root["push"].is_object())
    {
        const auto& push = root["push"];
        if (push.contains("vapid_public_key") && push["vapid_public_key"].is_string())
            out.push.vapid_public_key = push["vapid_public_key"].get<std::string>();
        if (push.contains("vapid_private_key") && push["vapid_private_key"].is_string())
            out.push.vapid_private_key = push["vapid_private_key"].get<std::string>();
        if (push.contains("subject") && push["subject"].is_string())
            out.push.subject = push["subject"].get<std::string>();
    }

    return true;
}

WAVE_NAMESPACE_END
