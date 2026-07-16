#include "app_config.h"

#include <fstream>

#include "../core/logger.h"

WAVE_NAMESPACE_BEGIN

namespace
{
    bool apply_config_section(const json& root, AppConfig& out)
    {
        if (!root.contains("server") || !root["server"].is_object())
        {
            LOG_ERROR("Config section is missing \"server\" object");
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

        assignPath("device_list_path", out.device_list_path);
        assignPath("gesture_sets_path", out.gesture_sets_path);
        assignPath("sleep_model_path", out.sleep_model_path);
        assignPath("stt_model_path", out.stt_model_path);
        assignPath("tts_model_path", out.tts_model_path);
        assignPath("anchor_date", out.anchor_date);

        if (root.contains("devices_enabled") && root["devices_enabled"].is_boolean())
            out.devices_enabled = root["devices_enabled"].get<bool>();

        const auto& server = out.server;
        if (server.contains("skip_migrations") && server["skip_migrations"].is_boolean())
            out.skip_db_migrations = server["skip_migrations"].get<bool>();
        if (server.contains("read_only") && server["read_only"].is_boolean())
            out.db_read_only = server["read_only"].get<bool>();

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

        if (root.contains("agent") && root["agent"].is_object())
        {
            const auto& agent = root["agent"];
            if (agent.contains("base_url") && agent["base_url"].is_string())
                out.agent.base_url = agent["base_url"].get<std::string>();
            if (agent.contains("embedding_model") && agent["embedding_model"].is_string())
                out.agent.embedding_model = agent["embedding_model"].get<std::string>();
        }

        return true;
    }
}

bool AppConfig::load_from_file(
    const std::filesystem::path& path,
    const std::string& profile,
    AppConfig& out)
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

    if (root.contains(profile) && root[profile].is_object())
        return apply_config_section(root[profile], out);

    if (root.contains("server") && root["server"].is_object())
    {
        LOG_INFO("Loading legacy flat config from {}", path.string());
        return apply_config_section(root, out);
    }

    LOG_ERROR(
        "Config file {} has no profile \"{}\" and is not a legacy flat config",
        path.string(),
        profile);
    return false;
}

WAVE_NAMESPACE_END
