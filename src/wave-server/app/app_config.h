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

    std::string log_level = "INFO";
    uint32_t io_threads_num = 4;

    json server;
    PushConfig push;

    std::string setting_path = "data/settings.json";
    std::string device_list_path = "device/device_list.json";
    std::string rooms_path = "device/rooms.json";
    std::string gesture_sets_path = "gestures/gesture_sets.json";
    std::string sleep_model_path = "models/sleep/model.json";
    std::string stt_model_path = "models/stt/stt.json";
    std::string tts_model_path = "models/tts/tts.json";
    bool devices_enabled = true;

    static bool loadFromFile(const std::filesystem::path& path, AppConfig& out);
};

WAVE_NAMESPACE_END
