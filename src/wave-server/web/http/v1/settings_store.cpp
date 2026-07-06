#include "settings_store.h"

#include <sstream>

#include "session_store.h"
#include "../../../core/time_util.h"
#include "../../../core/logger.h"

WAVE_NAMESPACE_BEGIN
namespace web {
namespace v1 {
namespace
{
    bool isValidTime(const std::string& value)
    {
        if (value.size() != 5 || value[2] != ':')
            return false;
        const int hours = std::stoi(value.substr(0, 2));
        const int minutes = std::stoi(value.substr(3, 2));
        return hours >= 0 && hours <= 23 && minutes >= 0 && minutes <= 59;
    }

    int minutesOf(const std::string& time)
    {
        return std::stoi(time.substr(0, 2)) * 60 + std::stoi(time.substr(3, 2));
    }

    bool parseJsonText(const std::string& text, Json::Value& out)
    {
        Json::CharReaderBuilder builder;
        std::string errors;
        std::istringstream stream(text);
        return Json::parseFromStream(builder, stream, &out, &errors);
    }

    std::string jsonToText(const Json::Value& value)
    {
        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";
        return Json::writeString(builder, value);
    }

    bool soundExists(const std::string& id)
    {
        return id == "sign-of-the-times" || id == "love-yourself";
    }

    bool speakerExists(int id)
    {
        return id >= 0 && id <= 9;
    }
}

SettingsStore::SettingsStore(drogon::orm::DbClientPtr client) :
    m_client(std::move(client))
{
}

std::optional<int64_t> SettingsStore::resolveActiveUserId(
    const SessionStore& sessions,
    const drogon::HttpRequestPtr& req) const
{
    const auto session = sessions.resolveSession(req);
    if (!session.active_account)
        return std::nullopt;
    return session.active_account->id;
}

Json::Value SettingsStore::defaultGeneralSettings() const
{
    Json::Value value;
    value["theme"] = "light";
    value["language"] = "ko";
    value["notificationSound"] = "sign-of-the-times";
    value["ttsSpeakerId"] = 0;
    value["ttsPlaybackSpeed"] = 1.0;
    value["browserPushEnabled"] = false;
    return value;
}

Json::Value SettingsStore::defaultSleepConfig() const
{
    Json::Value value;
    value["bedtime"] = "23:30";
    value["wakeTime"] = "07:00";
    value["wakeUpSound"] = "love-yourself";
    value["acAuto"] = true;
    value["acTemp"] = 24;
    value["lightAuto"] = true;
    value["dimStartMinutes"] = 30;
    value["finalBrightness"] = 10;
    value["wakeLightRamp"] = true;
    value["wakeMusic"] = true;
    value["wakeTvOrAlarm"] = false;
    return value;
}

Json::Value SettingsStore::loadJsonColumn(const char* table, const char* column, int64_t user_id) const
{
    const auto query = std::string("SELECT ") + column + " FROM " + table + " WHERE user_id = ? LIMIT 1";
    auto rows = m_client->execSqlSync(query, user_id);
    if (rows.empty())
        return Json::Value(Json::objectValue);

    Json::Value parsed;
    if (!parseJsonText(rows[0][column].as<std::string>(), parsed))
        return Json::Value(Json::objectValue);
    return parsed;
}

bool SettingsStore::upsertJsonColumn(
    const char* table,
    const char* column,
    int64_t user_id,
    const Json::Value& value) const
{
    const auto now = formatTimestamp();
    const auto text = jsonToText(value);
    auto existing = m_client->execSqlSync(
        std::string("SELECT user_id FROM ") + table + " WHERE user_id = ? LIMIT 1",
        user_id);
    if (existing.empty())
    {
        m_client->execSqlSync(
            std::string("INSERT INTO ") + table + " (user_id, " + column + ", updated_at) VALUES (?, ?, ?)",
            user_id,
            text,
            now);
    }
    else
    {
        m_client->execSqlSync(
            std::string("UPDATE ") + table + " SET " + column + " = ?, updated_at = ? WHERE user_id = ?",
            text,
            now,
            user_id);
    }
    return true;
}

Json::Value SettingsStore::getGeneralSettings(int64_t user_id) const
{
    Json::Value merged = defaultGeneralSettings();
    const auto stored = loadJsonColumn("user_general_settings", "settings", user_id);
    for (const auto& key : stored.getMemberNames())
        merged[key] = stored[key];
    return merged;
}

bool SettingsStore::putGeneralSettings(
    int64_t user_id,
    const Json::Value& body,
    Json::Value& out,
    std::string& error,
    std::string& field)
{
    out = getGeneralSettings(user_id);
    for (const auto& key : body.getMemberNames())
        out[key] = body[key];

    if (out.isMember("theme"))
    {
        const auto theme = out["theme"].asString();
        if (theme != "light" && theme != "dark" && theme != "system")
        {
            error = "지원하지 않는 테마입니다.";
            field = "theme";
            return false;
        }
    }

    if (out.isMember("language"))
    {
        const auto language = out["language"].asString();
        if (language != "ko" && language != "en")
        {
            error = "지원하지 않는 언어입니다.";
            field = "language";
            return false;
        }
    }

    if (out.isMember("ttsSpeakerId") && !speakerExists(out["ttsSpeakerId"].asInt()))
    {
        error = "존재하지 않는 TTS 스피커입니다.";
        field = "ttsSpeakerId";
        return false;
    }

    if (out.isMember("notificationSound") && !soundExists(out["notificationSound"].asString()))
    {
        error = "존재하지 않는 알림음입니다.";
        field = "notificationSound";
        return false;
    }

    if (out.isMember("ttsPlaybackSpeed"))
    {
        const auto speed = out["ttsPlaybackSpeed"].asDouble();
        if (speed < 0.5 || speed > 2.0)
        {
            error = "재생 속도는 0.5~2.0 사이여야 합니다.";
            field = "ttsPlaybackSpeed";
            return false;
        }
    }

    try
    {
        upsertJsonColumn("user_general_settings", "settings", user_id, out);
        return true;
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("Failed to save general settings: {}", e.what());
        error = "설정 저장에 실패했습니다.";
        return false;
    }
}

Json::Value SettingsStore::getSleepConfig(int64_t user_id) const
{
    Json::Value merged = defaultSleepConfig();
    const auto stored = loadJsonColumn("user_sleep_config", "config", user_id);
    for (const auto& key : stored.getMemberNames())
        merged[key] = stored[key];
    return merged;
}

bool SettingsStore::putSleepConfig(
    int64_t user_id,
    const Json::Value& body,
    Json::Value& out,
    std::string& error,
    std::string& field)
{
    out = getSleepConfig(user_id);
    for (const auto& key : body.getMemberNames())
        out[key] = body[key];

    if (!out.isMember("bedtime") || !out.isMember("wakeTime")
        || !isValidTime(out["bedtime"].asString()) || !isValidTime(out["wakeTime"].asString())
        || minutesOf(out["bedtime"].asString()) == minutesOf(out["wakeTime"].asString()))
    {
        error = "취침 시간과 기상 시간을 확인해주세요.";
        field = "bedtime";
        return false;
    }

    if (out.isMember("acTemp"))
    {
        const auto temp = out["acTemp"].asInt();
        if (temp < 20 || temp > 28)
        {
            error = "온도는 20~28 사이여야 합니다.";
            field = "acTemp";
            return false;
        }
    }

    if (out.isMember("dimStartMinutes"))
    {
        const auto minutes = out["dimStartMinutes"].asInt();
        if (minutes < 10 || minutes > 60)
        {
            error = "조명 조절 시작은 10~60분 사이여야 합니다.";
            field = "dimStartMinutes";
            return false;
        }
    }

    if (out.isMember("finalBrightness"))
    {
        const auto brightness = out["finalBrightness"].asInt();
        if (brightness < 0 || brightness > 30)
        {
            error = "최종 밝기는 0~30 사이여야 합니다.";
            field = "finalBrightness";
            return false;
        }
    }

    if (out.isMember("wakeUpSound") && !soundExists(out["wakeUpSound"].asString()))
    {
        error = "존재하지 않는 알람음입니다.";
        field = "wakeUpSound";
        return false;
    }

    try
    {
        upsertJsonColumn("user_sleep_config", "config", user_id, out);
        return true;
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("Failed to save sleep config: {}", e.what());
        error = "설정 저장에 실패했습니다.";
        return false;
    }
}

Json::Value SettingsStore::listSounds() const
{
    Json::Value sounds(Json::arrayValue);
    {
        Json::Value item;
        item["id"] = "sign-of-the-times";
        item["label"] = "Sign of the Times";
        sounds.append(item);
    }
    {
        Json::Value item;
        item["id"] = "love-yourself";
        item["label"] = "Love Yourself";
        sounds.append(item);
    }
    return sounds;
}

Json::Value SettingsStore::listTtsSpeakers() const
{
    Json::Value speakers(Json::arrayValue);
    const char* names[] = { "미선", "하은", "서윤", "유진", "수아", "준호", "민준", "성민", "도현", "현우" };
    for (int i = 0; i < 10; ++i)
    {
        Json::Value item;
        item["id"] = i;
        item["name"] = names[i];
        item["description"] = "Supertonic 3 ko-KR";
        item["character"] = "wave";
        item["gender"] = i < 5 ? "female" : "male";
        speakers.append(item);
    }
    return speakers;
}

} // namespace v1
} // namespace web
WAVE_NAMESPACE_END
