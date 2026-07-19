#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "../core/json.h"

WAVE_NAMESPACE_BEGIN

struct AppConfig
{
    struct PushConfig
    {
        std::string vapid_public_key;
        std::string vapid_private_key;
        std::string subject = "mailto:wavehome@local";
    };

    struct AgentConfig
    {
        std::string base_url = "http://127.0.0.1:8502";
        std::string embedding_model = "nomic-embed-text";
    };

    std::string log_level = "INFO";
    uint32_t io_threads_num = 4;

    json server;
    PushConfig push;
    AgentConfig agent;

    std::string device_list_path = "device/device_list.json";
    std::string gesture_sets_path = "gestures/gesture_sets.json";
    /** Shared IR command store (send / learn / ir_recv match). */
    std::string ir_list_path = "device/ir_list.json";
    std::string sleep_model_path = "models/sleep/model.json";
    std::string stt_model_path = "models/stt/stt.json";
    std::string tts_model_path = "models/tts/tts.json";
    bool devices_enabled = true;

    /** Demo profile: fixed calendar date for sleep/today APIs (YYYY-MM-DD). */
    std::string anchor_date;
    bool skip_db_migrations = false;
    bool db_read_only = false;

    static bool load_from_file(
        const std::filesystem::path& path,
        const std::string& profile,
        AppConfig& out);
};

WAVE_NAMESPACE_END
