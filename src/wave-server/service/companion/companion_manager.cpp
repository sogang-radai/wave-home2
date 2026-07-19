#include "companion_manager.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <utility>

#include <json/json.h>

#include "../../app/app_state.h"
#include "../../core/json.h"
#include "../../core/logger.h"
#include "../../device/device.h"
#include "../../device/device_manager.h"
#include "../../device/device_wire_id.hpp"
#include "../../device/interface/audio.h"
#include "../../device/platform/radai_ws.h"
#include "../../web/http/v1/chat_store.h"
#include "../../web/http/v1/iot_store.h"
#include "../../web/http/v1/settings_store.h"
#include "../agent_client.h"
#include "../sleep/sleep_manager.h"
#include "util/time_util.h"

WAVE_NAMESPACE_BEGIN
SERVICE_NAMESPACE_BEGIN

namespace
{
    constexpr auto kIdleConversationTimeout = std::chrono::minutes(1);
    constexpr auto kPostChimeDelay = std::chrono::milliseconds(400);
    constexpr const char* kChimeRelativePath = "audio/companion-chime.wav";

    struct PendingUtterance
    {
        std::string externalId;
        int64_t userId = 0;
        std::string text;
        std::optional<int64_t> conversationId;
        bool hasLastActivity = false;
        std::chrono::steady_clock::time_point lastActivity {};
    };

    dev::Device* find_device_by_external_id(const std::string& external_id)
    {
        const auto id = dev::parseDeviceID(external_id);
        if (id == 0)
            return nullptr;
        return AppState::get().deviceManager.findDevice(id);
    }

    bool parse_companion_flag(const std::string& settings_json)
    {
        if (settings_json.empty())
            return false;
        try
        {
            const auto j = json::parse(settings_json);
            if (!j.is_object() || !j.contains("companion"))
                return false;
            const auto& v = j.at("companion");
            if (v.is_boolean())
                return v.get<bool>();
            if (v.is_number_integer())
                return v.get<int>() != 0;
            if (v.is_string())
            {
                const auto s = v.get<std::string>();
                return s == "1" || s == "true" || s == "TRUE";
            }
            return false;
        }
        catch (...)
        {
            return false;
        }
    }

    std::vector<float> pcm16_to_float(const std::vector<int16_t>& pcm)
    {
        std::vector<float> out;
        out.reserve(pcm.size());
        for (const auto sample : pcm)
            out.push_back(static_cast<float>(sample) / 32768.0f);
        return out;
    }

    std::vector<int16_t> resample_linear_pcm16(
        const std::vector<int16_t>& input,
        int32_t src_rate,
        int32_t dst_rate)
    {
        if (input.empty() || src_rate <= 0 || dst_rate <= 0 || src_rate == dst_rate)
            return input;

        const double ratio = static_cast<double>(dst_rate) / static_cast<double>(src_rate);
        const auto out_count = static_cast<size_t>(std::llround(input.size() * ratio));
        std::vector<int16_t> out(out_count);
        for (size_t i = 0; i < out_count; ++i)
        {
            const double src_pos = static_cast<double>(i) / ratio;
            const auto idx = static_cast<size_t>(src_pos);
            const double frac = src_pos - static_cast<double>(idx);
            const int16_t a = input[std::min(idx, input.size() - 1)];
            const int16_t b = input[std::min(idx + 1, input.size() - 1)];
            out[i] = static_cast<int16_t>(std::lround(a + (b - a) * frac));
        }
        return out;
    }

    bool read_wav_pcm16(const std::filesystem::path& path, std::vector<int16_t>& out_pcm, int32_t& out_rate)
    {
        out_pcm.clear();
        out_rate = 0;

        std::ifstream in(path, std::ios::binary);
        if (!in)
            return false;

        auto read_u32 = [&](uint32_t& v) -> bool
        {
            unsigned char b[4] {};
            in.read(reinterpret_cast<char*>(b), 4);
            if (!in)
                return false;
            v = static_cast<uint32_t>(b[0])
                | (static_cast<uint32_t>(b[1]) << 8)
                | (static_cast<uint32_t>(b[2]) << 16)
                | (static_cast<uint32_t>(b[3]) << 24);
            return true;
        };

        auto read_u16 = [&](uint16_t& v) -> bool
        {
            unsigned char b[2] {};
            in.read(reinterpret_cast<char*>(b), 2);
            if (!in)
                return false;
            v = static_cast<uint16_t>(b[0]) | (static_cast<uint16_t>(b[1]) << 8);
            return true;
        };

        char riff[4] {};
        in.read(riff, 4);
        if (std::string(riff, 4) != "RIFF")
            return false;

        uint32_t riff_size = 0;
        if (!read_u32(riff_size))
            return false;
        (void)riff_size;

        char wave[4] {};
        in.read(wave, 4);
        if (std::string(wave, 4) != "WAVE")
            return false;

        uint16_t audio_format = 0;
        uint16_t channels = 0;
        uint32_t sample_rate = 0;
        uint16_t bits_per_sample = 0;
        uint32_t data_size = 0;
        std::streampos data_pos {};

        while (in)
        {
            char chunk_id[4] {};
            in.read(chunk_id, 4);
            if (!in)
                break;

            uint32_t chunk_size = 0;
            if (!read_u32(chunk_size))
                return false;

            const std::string id(chunk_id, 4);
            if (id == "fmt ")
            {
                if (!read_u16(audio_format) || !read_u16(channels) || !read_u32(sample_rate))
                    return false;
                uint32_t byte_rate = 0;
                uint16_t block_align = 0;
                if (!read_u32(byte_rate) || !read_u16(block_align) || !read_u16(bits_per_sample))
                    return false;
                (void)byte_rate;
                (void)block_align;
                if (chunk_size > 16)
                    in.seekg(static_cast<std::streamoff>(chunk_size - 16), std::ios::cur);
            }
            else if (id == "data")
            {
                data_size = chunk_size;
                data_pos = in.tellg();
                in.seekg(static_cast<std::streamoff>(chunk_size), std::ios::cur);
            }
            else
            {
                in.seekg(static_cast<std::streamoff>(chunk_size), std::ios::cur);
            }
        }

        if (audio_format != 1 || channels < 1 || bits_per_sample != 16 || data_size == 0)
            return false;

        in.clear();
        in.seekg(data_pos);
        const size_t sample_count = data_size / 2;
        out_pcm.resize(sample_count);
        in.read(reinterpret_cast<char*>(out_pcm.data()), static_cast<std::streamsize>(data_size));
        if (!in)
            return false;

        if (channels > 1)
        {
            std::vector<int16_t> mono;
            mono.reserve(sample_count / channels);
            for (size_t i = 0; i + channels <= out_pcm.size(); i += channels)
                mono.push_back(out_pcm[i]);
            out_pcm = std::move(mono);
        }

        out_rate = static_cast<int32_t>(sample_rate);
        return !out_pcm.empty() && out_rate > 0;
    }

    bool play_pcm16_on_radai_ws(dev::RadaiWs& wave_station, const std::vector<int16_t>& pcm, int32_t sample_rate)
    {
        if (pcm.empty() || sample_rate <= 0)
            return false;

        auto play = pcm;
        const auto& audio_cfg = wave_station.getAudioConfig();
        const auto sink_rate = static_cast<int32_t>(audio_cfg.sampleRate);
        if (sink_rate > 0 && sink_rate != sample_rate)
            play = resample_linear_pcm16(play, sample_rate, sink_rate);

        const int32_t play_rate = sink_rate > 0 ? sink_rate : sample_rate;
        const size_t channels = std::max<uint8_t>(1, audio_cfg.channels);
        const size_t frame_samples = std::max<size_t>(
            1,
            static_cast<size_t>(play_rate) * audio_cfg.frameDurationMs / 1000 * channels);

        const auto start = std::chrono::steady_clock::now();
        size_t samples_sent = 0;

        for (size_t offset = 0; offset < play.size(); offset += frame_samples)
        {
            const size_t count = std::min(frame_samples, play.size() - offset);
            dev::AudioFrame frame;
            frame.samples.assign(
                play.begin() + static_cast<std::ptrdiff_t>(offset),
                play.begin() + static_cast<std::ptrdiff_t>(offset + count));
            if (frame.samples.size() < frame_samples)
                frame.samples.resize(frame_samples, 0);

            if (!wave_station.playFrame(frame))
            {
                wave_station.stopPlayback();
                return false;
            }

            samples_sent += frame_samples;
            const auto target = start + std::chrono::microseconds(
                static_cast<int64_t>(samples_sent) * 1000000
                / static_cast<int64_t>(play_rate * channels));
            std::this_thread::sleep_until(target);
        }

        wave_station.stopPlayback();
        return true;
    }

    std::string trim_copy(const std::string& value)
    {
        const auto start = value.find_first_not_of(" \t\r\n");
        if (start == std::string::npos)
            return {};
        const auto end = value.find_last_not_of(" \t\r\n");
        return value.substr(start, end - start + 1);
    }

    void prepend_personal_prompt(std::vector<AgentChatMessage>& messages, int64_t user_id)
    {
        messages.erase(
            std::remove_if(
                messages.begin(),
                messages.end(),
                [](const AgentChatMessage& message) { return message.role == "system"; }),
            messages.end());

        auto client = AppState::get().db();
        if (!client)
            return;

        const auto agent = web::v1::SettingsStore(client).getAiAgentSettings(user_id);
        if (!agent.isMember("personalPrompt") || !agent["personalPrompt"].isString())
            return;
        auto prompt = trim_copy(agent["personalPrompt"].asString());
        if (prompt.empty())
            return;
        messages.insert(messages.begin(), {"system", std::move(prompt)});
    }
}

CompanionManager& CompanionManager::get()
{
    static CompanionManager instance;
    return instance;
}

CompanionManager::~CompanionManager()
{
    stop();
}

bool CompanionManager::loadChimeWav()
{
    if (m_chimeLoaded)
        return !m_chimePcm.empty();

    m_chimeLoaded = true;
    const auto path = AppState::get().resolvePath(kChimeRelativePath);
    if (!read_wav_pcm16(path, m_chimePcm, m_chimeSampleRate))
    {
        WLOG_WARN("Companion chime load failed: {}", path.string());
        m_chimePcm.clear();
        m_chimeSampleRate = 0;
        return false;
    }
    return true;
}

void CompanionManager::applyMicGainToStation(const std::string& external_id, float mic_gain)
{
    auto* device = find_device_by_external_id(external_id);
    auto* station = dynamic_cast<dev::RadaiWs*>(device);
    if (!station)
        return;
    station->setMicGain(mic_gain);
}

void CompanionManager::applyMicGainsFromDb()
{
    auto client = AppState::get().db();
    if (!client)
        return;

    auto rows = client->execSqlSync(
        R"SQL(
SELECT d.id, d.name,
       COALESCE(json_extract(d.settings_json, '$.mic_gain'), 1.0) AS mic_gain
FROM device d
WHERE d.class = 'wave_station' AND d.archived = 0
)SQL");

    for (const auto& row : rows)
    {
        const auto wire = dev::wireIdForDbRow(
            row["id"].as<int64_t>(),
            row["name"].as<std::string>());
        float gain = 1.0f;
        try
        {
            gain = static_cast<float>(row["mic_gain"].as<double>());
        }
        catch (...)
        {
            gain = 1.0f;
        }
        applyMicGainToStation(wire, gain);
    }
}

void CompanionManager::start()
{
    if (m_running.exchange(true))
        return;

    loadChimeWav();
    reconcile();
    applyMicGainsFromDb();

    m_worker = std::thread([this]() { runLoop(); });
    WLOG_INFO("CompanionManager started");
}

void CompanionManager::onDevicesReady()
{
    reconcile();
    applyMicGainsFromDb();
}

void CompanionManager::stop()
{
    if (!m_running.exchange(false))
        return;

    m_stopCv.notify_all();

    if (m_worker.joinable())
        m_worker.join();

    std::lock_guard lock(m_mutex);
    for (auto& [id, runtime] : m_stations)
    {
        (void)id;
        releaseSttSession(runtime, true);
        ensureMicSubscription(runtime, false);
    }
    m_stations.clear();
    WLOG_INFO("CompanionManager stopped");
}

void CompanionManager::runLoop()
{
    while (m_running.load(std::memory_order_acquire))
    {
        const auto tick_start = std::chrono::steady_clock::now();

        // Do not gate on AppState::running — companion starts in onDatabaseReady before
        // that flag is set, and device audio should begin as soon as the station is up.
        if (!AppState::get().no_devices)
            tickAll();

        const auto elapsed = std::chrono::steady_clock::now() - tick_start;
        const auto sleep_for = std::chrono::milliseconds(75) - elapsed;
        if (sleep_for > std::chrono::milliseconds(0))
        {
            std::unique_lock lock(m_stopMutex);
            m_stopCv.wait_for(lock, sleep_for, [this]()
            {
                return !m_running.load(std::memory_order_acquire);
            });
        }
    }
}

std::vector<CompanionStationRuntime> CompanionManager::loadStationConfigs()
{
    std::vector<CompanionStationRuntime> configs;
    auto client = AppState::get().db();
    if (!client)
        return configs;

    auto rows = client->execSqlSync(
        R"SQL(
SELECT d.id, d.name, d.settings_json, r.id AS room_id, ru.user_id
FROM device d
JOIN device_room_map drm ON drm.device_id = d.id
JOIN room r ON r.id = drm.room_id
JOIN room_user_map ru ON ru.room_id = r.id
WHERE d.class = 'wave_station'
  AND d.archived = 0
  AND d.enabled = 1
ORDER BY d.id, ru.user_id
)SQL");

    std::unordered_map<int64_t, bool> seen;
    for (const auto& row : rows)
    {
        const auto device_id = row["id"].as<int64_t>();
        if (seen[device_id])
            continue;
        seen[device_id] = true;

        CompanionStationRuntime runtime;
        runtime.externalId = dev::wireIdForDbRow(device_id, row["name"].as<std::string>());
        runtime.name = row["name"].as<std::string>();
        runtime.roomId = row["room_id"].as<int64_t>();
        runtime.userId = row["user_id"].as<int64_t>();
        runtime.companionEnabled = parse_companion_flag(
            row["settings_json"].isNull() ? std::string() : row["settings_json"].as<std::string>());
        configs.push_back(std::move(runtime));
    }
    return configs;
}

void CompanionManager::reconcile()
{
    auto loaded = loadStationConfigs();
    std::lock_guard lock(m_mutex);

    std::unordered_map<std::string, CompanionStationRuntime> next;
    for (auto& cfg : loaded)
    {
        auto it = m_stations.find(cfg.externalId);
        if (it != m_stations.end())
        {
            cfg.micSubscribed = it->second.micSubscribed;
            cfg.processing = it->second.processing;
            cfg.sttSessionId = std::move(it->second.sttSessionId);
            cfg.conversationId = it->second.conversationId;
            cfg.lastActivity = it->second.lastActivity;
            cfg.hasLastActivity = it->second.hasLastActivity;
            cfg.lastPartialText = std::move(it->second.lastPartialText);
            cfg.lastFinalText = std::move(it->second.lastFinalText);
        }
        next.emplace(cfg.externalId, std::move(cfg));
    }

    for (auto& [id, runtime] : m_stations)
    {
        if (next.find(id) != next.end())
            continue;
        releaseSttSession(runtime, true);
        ensureMicSubscription(runtime, false);
    }

    m_stations = std::move(next);
}

void CompanionManager::notifyDeviceUpdated(
    const std::string& external_id,
    bool companion_enabled,
    std::optional<float> mic_gain)
{
    if (external_id.empty())
        return;

    if (mic_gain)
        applyMicGainToStation(external_id, *mic_gain);

    {
        std::lock_guard lock(m_mutex);
        auto it = m_stations.find(external_id);
        if (it != m_stations.end())
        {
            it->second.companionEnabled = companion_enabled;
            if (!companion_enabled)
            {
                releaseSttSession(it->second, true);
                ensureMicSubscription(it->second, false);
                it->second.conversationId.reset();
                it->second.hasLastActivity = false;
                it->second.lastPartialText.clear();
            }
            WLOG_INFO(
                "Companion mode {}: {} user={} room={}",
                companion_enabled ? "ON" : "OFF",
                external_id,
                it->second.userId,
                it->second.roomId);
            return;
        }
    }

    reconcile();

    std::lock_guard lock(m_mutex);
    auto it = m_stations.find(external_id);
    if (it != m_stations.end())
    {
        it->second.companionEnabled = companion_enabled;
        WLOG_INFO(
            "Companion mode {}: {} user={} room={}",
            companion_enabled ? "ON" : "OFF",
            external_id,
            it->second.userId,
            it->second.roomId);
    }
    else
    {
        WLOG_WARN(
            "Companion mode notify ignored — station not mapped to a room/user: {} "
            "(stations={})",
            external_id,
            m_stations.size());
    }
}

CompanionListenStatus CompanionManager::listenStatus(const std::string& external_id) const
{
    CompanionListenStatus status;
    std::lock_guard lock(m_mutex);
    const auto it = m_stations.find(external_id);
    if (it == m_stations.end())
        return status;

    status.enabled = it->second.companionEnabled;
    status.listening = it->second.companionEnabled && !it->second.processing;
    status.processing = it->second.processing;
    status.partialText = it->second.lastPartialText;
    status.finalText = it->second.lastFinalText;
    return status;
}

void CompanionManager::ensureMicSubscription(CompanionStationRuntime& runtime, bool want_subscribed)
{
    auto* device = find_device_by_external_id(runtime.externalId);
    auto* station = dynamic_cast<dev::RadaiWs*>(device);
    if (!station || !device || device->getState() != dev::DeviceState::Running)
    {
        runtime.micSubscribed = false;
        return;
    }

    if (want_subscribed)
    {
        // IoT UI / telemetry may already own the mic subscription — treat that as OK.
        const auto& sub = station->getSubscriptionState();
        if (sub.micOpus || sub.micPcm)
        {
            runtime.micSubscribed = true;
            return;
        }

        // Match RadaiWs::ensureMicSubscription — Opus mic with compressed flag.
        const auto& caps = station->getCapabilities();
        const auto& audio_cfg = station->getAudioConfig();
        int rc = -5;
        if (audio_cfg.preferCompressedMic && caps.micOpus)
        {
            rc = station->invoke(
                "subscribe",
                json{
                    {"target", "mic_opus"},
                    {"intervalMs", 0},
                    {"compressed", true},
                });
        }
        else if (caps.micPcm)
        {
            rc = station->invoke(
                "subscribe",
                json{{"target", "mic_pcm"}, {"intervalMs", 0}});
        }

        if (rc == 0)
        {
            runtime.micSubscribed = true;
        }
        else
        {
            runtime.micSubscribed = false;
            WLOG_WARN("Companion mic subscribe failed: {} rc={}", runtime.externalId, rc);
        }
    }
    else if (runtime.micSubscribed)
    {
        // Leave mic subscribed for IoT UI level meter; only clear our ownership flag.
        runtime.micSubscribed = false;
    }
}

void CompanionManager::drainMic(const std::string& external_id)
{
    auto* device = find_device_by_external_id(external_id);
    auto* audio = dynamic_cast<dev::IAudioInput*>(device);
    if (!audio)
        return;

    dev::AudioFrame frame;
    int pulled = 0;
    while (pulled < 64 && audio->popFrame(frame))
        ++pulled;
}

#ifdef WAVE_BUILD_TTS
void CompanionManager::ensureSttSession(CompanionStationRuntime& runtime)
{
    if (!runtime.sttSessionId.empty())
        return;

    if (!AppState::get().stt.isReady())
        return;

    std::string session_id;
    std::string code;
    if (!AppState::get().stt.createSession("ko-KR", session_id, code))
    {
        WLOG_WARN("Companion STT create failed: {} ({})", runtime.externalId, code);
        return;
    }
    runtime.sttSessionId = std::move(session_id);
}

void CompanionManager::releaseSttSession(CompanionStationRuntime& runtime, bool abort)
{
    if (runtime.sttSessionId.empty())
        return;

    std::string code;
    if (abort)
        AppState::get().stt.abortSession(runtime.sttSessionId, code);
    else
        AppState::get().stt.endSession(runtime.sttSessionId, code);
    runtime.sttSessionId.clear();
}
#else
void CompanionManager::ensureSttSession(CompanionStationRuntime& /*runtime*/)
{
}

void CompanionManager::releaseSttSession(CompanionStationRuntime& runtime, bool /*abort*/)
{
    runtime.sttSessionId.clear();
}
#endif

bool CompanionManager::playChime(const std::string& external_id)
{
    if (!loadChimeWav() || m_chimePcm.empty())
        return false;

    auto* device = find_device_by_external_id(external_id);
    auto* station = dynamic_cast<dev::RadaiWs*>(device);
    if (!station || !device || device->getState() != dev::DeviceState::Running)
        return false;

    return play_pcm16_on_radai_ws(*station, m_chimePcm, m_chimeSampleRate);
}

void CompanionManager::processUtterance(
    const std::string& external_id,
    int64_t user_id,
    const std::string& text,
    std::optional<int64_t> conversation_id,
    bool has_last_activity,
    std::chrono::steady_clock::time_point last_activity)
{
    WLOG_INFO("Companion recognized [{}]: \"{}\"", external_id, text);
    {
        std::lock_guard lock(m_mutex);
        auto it = m_stations.find(external_id);
        if (it != m_stations.end())
        {
            it->second.lastFinalText = text;
            it->second.lastPartialText = text;
        }
    }

    if (!playChime(external_id))
        WLOG_WARN("Companion chime playback failed: {}", external_id);
    std::this_thread::sleep_for(kPostChimeDelay);

    int64_t chat_id = 0;
    bool reuse = false;
    if (conversation_id && has_last_activity
        && (std::chrono::steady_clock::now() - last_activity) < kIdleConversationTimeout)
    {
        chat_id = *conversation_id;
        reuse = true;
    }

    auto client = AppState::get().db();
    if (!client)
    {
        std::lock_guard lock(m_mutex);
        auto it = m_stations.find(external_id);
        if (it != m_stations.end())
        {
            it->second.processing = false;
            drainMic(external_id);
        }
        return;
    }

    web::v1::ChatStore store(client);
    std::string error;
    std::string field;
    std::optional<Json::Value> appended;

    if (reuse)
    {
        appended = store.appendUserMessage(user_id, chat_id, text, error, field);
        if (!appended)
            reuse = false;
    }

    if (!reuse)
    {
        const auto title = web::v1::ChatStore::title_from_text(text);
        auto created = store.createConversation(user_id, title.empty() ? "동반자" : title);
        if (!created)
        {
            WLOG_WARN("Companion createConversation failed: {}", external_id);
            std::lock_guard lock(m_mutex);
            auto it = m_stations.find(external_id);
            if (it != m_stations.end())
            {
                it->second.processing = false;
                drainMic(external_id);
            }
            return;
        }
        chat_id = (*created)["id"].asInt64();
        appended = store.appendUserMessage(user_id, chat_id, text, error, field);
    }

    if (!appended)
    {
        WLOG_WARN("Companion appendUserMessage failed: {} ({})", external_id, error);
        std::lock_guard lock(m_mutex);
        auto it = m_stations.find(external_id);
        if (it != m_stations.end())
        {
            it->second.processing = false;
            drainMic(external_id);
        }
        return;
    }

#ifdef WAVE_BUILD_TTS
    auto agent_messages = store.buildAgentMessages((*appended)["messages"]);
    prepend_personal_prompt(agent_messages, user_id);

    AgentChatTurnRequest request;
    request.chat_history_id = chat_id;
    request.user_id = user_id;
    request.messages = std::move(agent_messages);
    request.now = formatTimestamp();
    request.stream = true;

    std::string assistant_text;
    std::string model;
    std::string agent_error;
    std::string reasoning;
    json tool_events = json::array();
    const auto agent_result = runChatTurnSync(
        AppState::get().config.agent.base_url,
        request,
        assistant_text,
        model,
        agent_error,
        &tool_events,
        &reasoning);

    if (agent_result == AgentClientResult::success && !assistant_text.empty())
    {
        Json::Value tool_events_jc(Json::arrayValue);
        {
            Json::CharReaderBuilder builder;
            std::string errors;
            const auto text = tool_events.dump();
            std::istringstream stream(text);
            Json::Value parsed;
            if (Json::parseFromStream(builder, stream, &parsed, &errors) && parsed.isArray())
                tool_events_jc = std::move(parsed);
        }

        const int64_t assistant_id = store.nextMessageId((*appended)["messages"]);
        store.appendAssistantMessage(
            user_id, chat_id, assistant_id, assistant_text, tool_events_jc, reasoning);

        std::string tts_code;
        web::v1::IotStore iot(AppState::get().deviceManager);
        if (!iot.sendDeviceTts(external_id, assistant_text, 0, 1.0f, tts_code))
            WLOG_WARN("Companion TTS failed: {} ({})", external_id, tts_code);
    }
    else
    {
        WLOG_WARN("Companion agent failed: {} ({})", external_id, agent_error);
    }
#else
    (void)chat_id;
#endif

    std::lock_guard lock(m_mutex);
    auto it = m_stations.find(external_id);
    if (it != m_stations.end())
    {
        it->second.processing = false;
        it->second.conversationId = chat_id;
        it->second.lastActivity = std::chrono::steady_clock::now();
        it->second.hasLastActivity = true;
        drainMic(external_id);
    }
}

void CompanionManager::tickAll()
{
    std::vector<std::string> station_ids;
    {
        std::lock_guard lock(m_mutex);
        station_ids.reserve(m_stations.size());
        for (const auto& [id, runtime] : m_stations)
        {
            (void)runtime;
            station_ids.push_back(id);
        }
    }

    std::vector<PendingUtterance> pending;

    for (const auto& external_id : station_ids)
    {
        bool companion_enabled = false;
        int64_t user_id = 0;
        bool processing = false;
        bool had_resources = false;
        {
            std::lock_guard lock(m_mutex);
            auto it = m_stations.find(external_id);
            if (it == m_stations.end())
                continue;
            companion_enabled = it->second.companionEnabled;
            user_id = it->second.userId;
            processing = it->second.processing;
            had_resources = it->second.micSubscribed || !it->second.sttSessionId.empty();
        }

        if (!companion_enabled || user_id <= 0)
        {
            if (had_resources)
            {
                std::lock_guard lock(m_mutex);
                auto it = m_stations.find(external_id);
                if (it != m_stations.end())
                {
                    releaseSttSession(it->second, true);
                    ensureMicSubscription(it->second, false);
                }
            }
            continue;
        }

        auto* device = find_device_by_external_id(external_id);
        if (!device || device->getState() != dev::DeviceState::Running)
        {
            std::lock_guard lock(m_mutex);
            auto it = m_stations.find(external_id);
            if (it == m_stations.end())
                continue;
            releaseSttSession(it->second, true);
            ensureMicSubscription(it->second, false);
            continue;
        }

        // Outside companion mutex — sleep tick may hold its lock across device I/O.
        if (SleepManager::get().isStationMicInUse(external_id))
        {
            std::lock_guard lock(m_mutex);
            auto it = m_stations.find(external_id);
            if (it == m_stations.end())
                continue;
            releaseSttSession(it->second, true);
            ensureMicSubscription(it->second, false);
            continue;
        }

        if (processing)
            continue;

#ifdef WAVE_BUILD_TTS
        CompanionStationRuntime local;
        {
            std::lock_guard lock(m_mutex);
            auto it = m_stations.find(external_id);
            if (it == m_stations.end() || it->second.processing)
                continue;
            local = it->second;
        }

        // Mic subscribe / STT create can wait on device or model init — keep mutex free.
        ensureMicSubscription(local, true);
        ensureSttSession(local);

        {
            std::lock_guard lock(m_mutex);
            auto it = m_stations.find(external_id);
            if (it == m_stations.end() || it->second.processing)
            {
                // Drop session created on a stale snapshot so STT stays available.
                if (!local.sttSessionId.empty()
                    && (it == m_stations.end()
                        || it->second.sttSessionId != local.sttSessionId))
                {
                    releaseSttSession(local, true);
                }
                continue;
            }
            it->second.micSubscribed = local.micSubscribed;
            it->second.sttSessionId = std::move(local.sttSessionId);
            if (it->second.sttSessionId.empty())
                continue;
            local.sttSessionId = it->second.sttSessionId;
            local.micSubscribed = it->second.micSubscribed;
        }

        const std::string stt_session_id = local.sttSessionId;

        auto* audio = dynamic_cast<dev::IAudioInput*>(device);
        if (!audio)
            continue;

        const auto fmt = audio->getSourceFormat();
        const uint32_t sample_rate = fmt.sampleRate > 0 ? fmt.sampleRate : 16000;
        const uint32_t channels = fmt.channels > 0 ? fmt.channels : 1;

        dev::AudioFrame frame;
        int pulled = 0;
        while (pulled < 32 && audio->popFrame(frame))
        {
            if (!frame.samples.empty())
            {
                std::vector<float> floats;
                if (channels <= 1)
                {
                    floats = pcm16_to_float(frame.samples);
                }
                else
                {
                    const size_t frames = frame.samples.size() / channels;
                    floats.resize(frames);
                    for (size_t i = 0; i < frames; ++i)
                    {
                        int32_t sum = 0;
                        for (uint32_t c = 0; c < channels; ++c)
                            sum += frame.samples[i * channels + c];
                        floats[i] = static_cast<float>(sum)
                            / static_cast<float>(channels) / 32768.0f;
                    }
                }

                std::string code;
                if (!AppState::get().stt.pushAudio(
                        stt_session_id,
                        floats.data(),
                        floats.size(),
                        sample_rate,
                        code))
                {
                    WLOG_WARN(
                        "Companion STT pushAudio failed: {} ({})",
                        external_id,
                        code);
                    if (code == "NOT_FOUND")
                    {
                        std::lock_guard lock(m_mutex);
                        auto it = m_stations.find(external_id);
                        if (it != m_stations.end())
                            it->second.sttSessionId.clear();
                    }
                    break;
                }
            }
            ++pulled;
        }

        std::string last_partial;
        {
            std::lock_guard lock(m_mutex);
            auto it = m_stations.find(external_id);
            if (it != m_stations.end())
                last_partial = it->second.lastPartialText;
        }

        // popEvent returns true even when the queue is empty (null event).
        for (;;)
        {
            Json::Value event;
            bool closed = false;
            std::string code;
            if (!AppState::get().stt.popEvent(
                    stt_session_id,
                    event,
                    closed,
                    std::chrono::milliseconds(0),
                    code))
            {
                std::lock_guard lock(m_mutex);
                auto it = m_stations.find(external_id);
                if (it != m_stations.end())
                    it->second.sttSessionId.clear();
                break;
            }

            if (!event.isObject() || !event.isMember("type"))
            {
                if (closed)
                {
                    std::lock_guard lock(m_mutex);
                    auto it = m_stations.find(external_id);
                    if (it != m_stations.end())
                        it->second.sttSessionId.clear();
                }
                break;
            }

            if (event["type"].asString() == "done")
            {
                std::lock_guard lock(m_mutex);
                auto it = m_stations.find(external_id);
                if (it != m_stations.end())
                    it->second.sttSessionId.clear();
                break;
            }

            if (event["type"].asString() != "partial")
                continue;

            const auto text =
                event.isMember("text") ? trim_copy(event["text"].asString()) : std::string();
            const bool is_endpoint =
                event.isMember("isEndpoint") && event["isEndpoint"].asBool();

            if (!text.empty() && text != last_partial)
            {
                last_partial = text;
                std::lock_guard lock(m_mutex);
                auto it = m_stations.find(external_id);
                if (it != m_stations.end())
                    it->second.lastPartialText = text;
            }

            if (!is_endpoint || text.empty())
                continue;

            PendingUtterance utterance;
            {
                std::lock_guard lock(m_mutex);
                auto it = m_stations.find(external_id);
                if (it == m_stations.end())
                    break;
                utterance.externalId = external_id;
                utterance.userId = it->second.userId;
                utterance.text = text;
                utterance.conversationId = it->second.conversationId;
                utterance.hasLastActivity = it->second.hasLastActivity;
                utterance.lastActivity = it->second.lastActivity;

                it->second.processing = true;
                it->second.lastPartialText.clear();
                releaseSttSession(it->second, true);
                ensureMicSubscription(it->second, false);
            }
            pending.push_back(std::move(utterance));
            break;
        }
#else
        (void)external_id;
#endif
    }

    for (const auto& utterance : pending)
    {
        processUtterance(
            utterance.externalId,
            utterance.userId,
            utterance.text,
            utterance.conversationId,
            utterance.hasLastActivity,
            utterance.lastActivity);
    }
}

SERVICE_NAMESPACE_END
WAVE_NAMESPACE_END
