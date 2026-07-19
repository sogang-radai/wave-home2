#include "chat_store.h"
#include "../../../db/database.h"

#include <sstream>

#include "util/time_util.h"
#include "../../../service/agent/agent_client.h"

WAVE_NAMESPACE_BEGIN
namespace web {
namespace v1 {

namespace
{
    std::string trim_copy(const std::string& value)
    {
        const auto start = value.find_first_not_of(" \t\n\r");
        if (start == std::string::npos)
            return {};
        const auto end = value.find_last_not_of(" \t\n\r");
        return value.substr(start, end - start + 1);
    }

    size_t utf8_sequence_length(unsigned char lead)
    {
        if (lead < 0x80)
            return 1;
        if ((lead & 0xE0) == 0xC0)
            return 2;
        if ((lead & 0xF0) == 0xE0)
            return 3;
        if ((lead & 0xF8) == 0xF0)
            return 4;
        return 0;
    }

    size_t utf8_code_point_count(const std::string& text)
    {
        size_t count = 0;
        for (size_t i = 0; i < text.size();)
        {
            const auto len = utf8_sequence_length(static_cast<unsigned char>(text[i]));
            if (len == 0 || i + len > text.size())
                break;
            i += len;
            ++count;
        }
        return count;
    }

    std::string truncate_utf8(const std::string& text, size_t max_code_points)
    {
        size_t byte_end = 0;
        size_t count = 0;
        while (byte_end < text.size() && count < max_code_points)
        {
            const auto len = utf8_sequence_length(static_cast<unsigned char>(text[byte_end]));
            if (len == 0 || byte_end + len > text.size())
                break;
            byte_end += len;
            ++count;
        }
        if (byte_end >= text.size())
            return text;
        return text.substr(0, byte_end) + "…";
    }
}

ChatStore::ChatStore(db::DbClientPtr client) :
    m_client(std::move(client))
{
}

std::string ChatStore::to_created_at_iso(const std::string& db_time)
{
    if (db_time.size() >= 19)
        return db_time.substr(0, 10) + "T" + db_time.substr(11, 8) + "+09:00";
    return db_time;
}

std::string ChatStore::title_from_text(const std::string& text)
{
    constexpr size_t kMaxTitleChars = 22;
    const auto trimmed = trim_copy(text);
    if (trimmed.empty())
        return "새 대화";
    if (utf8_code_point_count(trimmed) <= kMaxTitleChars)
        return trimmed;
    return truncate_utf8(trimmed, kMaxTitleChars);
}

int64_t ChatStore::nextConversationId() const
{
    auto rows = m_client->execSqlSync("SELECT COALESCE(MAX(id), 0) + 1 AS next_id FROM chat_history");
    return rows.empty() ? 1 : rows[0]["next_id"].as<int64_t>();
}

int64_t ChatStore::nextMessageId(const Json::Value& messages) const
{
    int64_t max_id = 0;
    if (!messages.isArray())
        return 1;

    for (const auto& message : messages)
    {
        if (message.isMember("id") && message["id"].isInt64())
            max_id = std::max(max_id, message["id"].asInt64());
        else if (message.isMember("id") && message["id"].isInt())
            max_id = std::max(max_id, static_cast<int64_t>(message["id"].asInt()));
    }
    return max_id + 1;
}

Json::Value ChatStore::normalize_messages_json(Json::Value raw, const std::string& fallback_db_time)
{
    if (raw.isObject() && raw.isMember("messages"))
        raw = raw["messages"];
    if (!raw.isArray())
        return Json::Value(Json::arrayValue);

    Json::Value out(Json::arrayValue);
    int64_t next_id = 1;
    const std::string created_at = fallback_db_time.empty()
        ? to_created_at_iso("")
        : to_created_at_iso(fallback_db_time);

    for (const auto& entry : raw)
    {
        if (!entry.isObject())
            continue;

        const std::string role = entry.get("role", "").asString();
        if (role != "user" && role != "assistant")
            continue;

        std::string text;
        if (entry.isMember("text") && entry["text"].isString())
            text = entry["text"].asString();
        else if (entry.isMember("content") && entry["content"].isString())
            text = entry["content"].asString();
        if (text.empty())
            continue;

        Json::Value message;
        if (entry.isMember("id") && entry["id"].isIntegral())
            message["id"] = entry["id"];
        else
            message["id"] = static_cast<Json::Int64>(next_id++);
        message["role"] = role;
        message["text"] = text;
        if (entry.isMember("createdAt") && entry["createdAt"].isString())
            message["createdAt"] = entry["createdAt"].asString();
        else
            message["createdAt"] = created_at;
        if (role == "assistant")
        {
            if (entry.isMember("status"))
                message["status"] = entry["status"];
            else
                message["status"] = "done";
            if (entry.isMember("toolEvents") && entry["toolEvents"].isArray())
                message["toolEvents"] = entry["toolEvents"];
            else
                message["toolEvents"] = Json::Value(Json::arrayValue);
            if (entry.isMember("reasoning"))
                message["reasoning"] = entry["reasoning"];
        }
        out.append(message);
    }
    return out;
}

Json::Value ChatStore::parseMessagesColumn(const std::string& raw) const
{
    if (raw.empty())
        return Json::Value(Json::arrayValue);

    Json::Value out;
    Json::CharReaderBuilder builder;
    std::string errors;
    std::istringstream stream(raw);
    if (!Json::parseFromStream(builder, stream, &out, &errors))
        return Json::Value(Json::arrayValue);
    return normalize_messages_json(std::move(out));
}

std::string ChatStore::serializeMessages(const Json::Value& messages) const
{
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, messages);
}

Json::Value ChatStore::makeUserMessage(int64_t message_id, const std::string& text, const std::string& created_at) const
{
    Json::Value message;
    message["id"] = static_cast<Json::Int64>(message_id);
    message["role"] = "user";
    message["text"] = text;
    message["createdAt"] = to_created_at_iso(created_at);
    return message;
}

Json::Value ChatStore::makeAssistantShell(int64_t message_id, const std::string& created_at) const
{
    Json::Value message;
    message["id"] = static_cast<Json::Int64>(message_id);
    message["role"] = "assistant";
    message["text"] = "";
    message["status"] = "streaming";
    message["reasoning"] = Json::nullValue;
    message["toolEvents"] = Json::Value(Json::arrayValue);
    message["createdAt"] = to_created_at_iso(created_at);
    return message;
}

Json::Value ChatStore::toSummary(
    int64_t id,
    const std::string& title,
    const Json::Value& messages,
    const std::string& created_at,
    const std::string& updated_at) const
{
    Json::Value summary;
    summary["id"] = static_cast<Json::Int64>(id);
    summary["title"] = title;
    summary["messageCount"] = static_cast<Json::UInt>(messages.size());

    std::string preview;
    if (messages.isArray() && !messages.empty())
    {
        const auto& last = messages[static_cast<Json::ArrayIndex>(messages.size() - 1u)];
        if (last.isMember("text") && last["text"].isString())
            preview = last["text"].asString();
    }
    summary["lastMessagePreview"] = preview.empty() ? Json::nullValue : Json::Value(preview);
    summary["createdAt"] = to_created_at_iso(created_at);
    summary["updatedAt"] = to_created_at_iso(updated_at);
    return summary;
}

std::optional<Json::Value> ChatStore::loadConversationRow(int64_t user_id, int64_t conversation_id) const
{
    auto rows = m_client->execSqlSync(
        R"SQL(
SELECT id, title, created_at, updated_at, message
FROM chat_history
WHERE id = ? AND user_id = ?
)SQL",
        conversation_id,
        user_id);

    if (rows.empty())
        return std::nullopt;

    const auto& row = rows[0];
    Json::Value conversation;
    conversation["id"] = static_cast<Json::Int64>(row["id"].as<int64_t>());
    conversation["title"] = row["title"].as<std::string>();
    conversation["createdAt"] = to_created_at_iso(row["created_at"].as<std::string>());
    conversation["updatedAt"] = to_created_at_iso(row["updated_at"].as<std::string>());
    conversation["messages"] = parseMessagesColumn(row["message"].as<std::string>());
    return conversation;
}

bool ChatStore::saveMessages(
    int64_t conversation_id,
    const Json::Value& messages,
    const std::string& updated_at) const
{
    m_client->execSqlSync(
        "UPDATE chat_history SET message = ?, updated_at = ? WHERE id = ?",
        serializeMessages(messages),
        updated_at,
        conversation_id);
    return true;
}

Json::Value ChatStore::listSummaries(int64_t user_id) const
{
    Json::Value body(Json::arrayValue);
    auto rows = m_client->execSqlSync(
        R"SQL(
SELECT id, title, created_at, updated_at, message
FROM chat_history
WHERE user_id = ?
ORDER BY updated_at DESC, id DESC
)SQL",
        user_id);

    for (const auto& row : rows)
    {
        const auto messages = parseMessagesColumn(row["message"].as<std::string>());
        body.append(toSummary(
            row["id"].as<int64_t>(),
            row["title"].as<std::string>(),
            messages,
            row["created_at"].as<std::string>(),
            row["updated_at"].as<std::string>()));
    }
    return body;
}

std::optional<Json::Value> ChatStore::getConversation(int64_t user_id, int64_t conversation_id) const
{
    return loadConversationRow(user_id, conversation_id);
}

std::optional<Json::Value> ChatStore::createConversation(int64_t user_id, const std::string& title) const
{
    const auto trimmed = trim_copy(title);
    if (trimmed.empty())
        return std::nullopt;

    const auto id = nextConversationId();
    const auto now = formatTimestamp();
    m_client->execSqlSync(
        R"SQL(
INSERT INTO chat_history (id, user_id, title, created_at, updated_at, message)
VALUES (?, ?, ?, ?, ?, '[]')
)SQL",
        id,
        user_id,
        trimmed,
        now,
        now);

    Json::Value conversation;
    conversation["id"] = static_cast<Json::Int64>(id);
    conversation["title"] = trimmed;
    conversation["messages"] = Json::Value(Json::arrayValue);
    conversation["createdAt"] = to_created_at_iso(now);
    conversation["updatedAt"] = to_created_at_iso(now);
    return conversation;
}

std::optional<Json::Value> ChatStore::renameConversation(
    int64_t user_id,
    int64_t conversation_id,
    const std::string& title,
    std::string& error,
    std::string& field) const
{
    const auto trimmed = trim_copy(title);
    if (trimmed.empty())
    {
        error = "대화 제목을 입력해주세요.";
        field = "title";
        return std::nullopt;
    }

    auto conversation = loadConversationRow(user_id, conversation_id);
    if (!conversation)
    {
        error = "대화를 찾을 수 없습니다.";
        return std::nullopt;
    }

    m_client->execSqlSync(
        "UPDATE chat_history SET title = ? WHERE id = ? AND user_id = ?",
        trimmed,
        conversation_id,
        user_id);

    auto rows = m_client->execSqlSync(
        R"SQL(
SELECT title, created_at, updated_at, message
FROM chat_history
WHERE id = ? AND user_id = ?
)SQL",
        conversation_id,
        user_id);
    if (rows.empty())
    {
        error = "대화를 찾을 수 없습니다.";
        return std::nullopt;
    }

    const auto& row = rows[0];
    const auto messages = parseMessagesColumn(row["message"].as<std::string>());
    return toSummary(
        conversation_id,
        row["title"].as<std::string>(),
        messages,
        row["created_at"].as<std::string>(),
        row["updated_at"].as<std::string>());
}

bool ChatStore::deleteConversation(int64_t user_id, int64_t conversation_id) const
{
    auto rows = m_client->execSqlSync(
        "DELETE FROM chat_history WHERE id = ? AND user_id = ?",
        conversation_id,
        user_id);
    return rows.affectedRows() > 0;
}

std::optional<Json::Value> ChatStore::appendUserMessage(
    int64_t user_id,
    int64_t conversation_id,
    const std::string& text,
    std::string& error,
    std::string& field) const
{
    const auto trimmed = trim_copy(text);
    if (trimmed.empty())
    {
        error = "메시지를 입력해주세요.";
        field = "text";
        return std::nullopt;
    }
    if (trimmed.size() > 2000)
    {
        error = "메시지는 2000자 이하로 입력해주세요.";
        field = "text";
        return std::nullopt;
    }

    auto conversation = loadConversationRow(user_id, conversation_id);
    if (!conversation)
    {
        error = "대화를 찾을 수 없습니다.";
        return std::nullopt;
    }

    const auto now = formatTimestamp();
    auto messages = (*conversation)["messages"];
    const auto message_id = nextMessageId(messages);
    messages.append(makeUserMessage(message_id, trimmed, now));
    saveMessages(conversation_id, messages, now);

    Json::Value result;
    result["conversationId"] = static_cast<Json::Int64>(conversation_id);
    result["userMessage"] = makeUserMessage(message_id, trimmed, now);
    result["messages"] = messages;
    result["updatedAt"] = to_created_at_iso(now);
    return result;
}

std::optional<Json::Value> ChatStore::createConversationWithUserMessage(
    const std::string& text,
    int64_t user_id,
    std::string& error,
    std::string& field) const
{
    const auto trimmed = trim_copy(text);
    if (trimmed.empty())
    {
        error = "메시지를 입력해주세요.";
        field = "text";
        return std::nullopt;
    }
    if (trimmed.size() > 2000)
    {
        error = "메시지는 2000자 이하로 입력해주세요.";
        field = "text";
        return std::nullopt;
    }

    const auto id = nextConversationId();
    const auto now = formatTimestamp();
    const auto title = title_from_text(trimmed);
    const auto message_id = static_cast<int64_t>(1);
    Json::Value messages(Json::arrayValue);
    messages.append(makeUserMessage(message_id, trimmed, now));

    m_client->execSqlSync(
        R"SQL(
INSERT INTO chat_history (id, user_id, title, created_at, updated_at, message)
VALUES (?, ?, ?, ?, ?, ?)
)SQL",
        id,
        user_id,
        title,
        now,
        now,
        serializeMessages(messages));

    Json::Value conversation;
    conversation["id"] = static_cast<Json::Int64>(id);
    conversation["title"] = title;
    conversation["messages"] = messages;
    conversation["createdAt"] = to_created_at_iso(now);
    conversation["updatedAt"] = to_created_at_iso(now);
    conversation["userMessage"] = makeUserMessage(message_id, trimmed, now);
    return conversation;
}

bool ChatStore::appendAssistantMessage(
    int64_t user_id,
    int64_t conversation_id,
    int64_t message_id,
    const std::string& text,
    const Json::Value& tool_events,
    const std::string& reasoning) const
{
    auto conversation = loadConversationRow(user_id, conversation_id);
    if (!conversation)
        return false;

    const auto now = formatTimestamp();
    auto messages = (*conversation)["messages"];

    Json::Value assistant;
    assistant["id"] = static_cast<Json::Int64>(message_id);
    assistant["role"] = "assistant";
    assistant["text"] = text;
    assistant["status"] = "done";
    assistant["toolEvents"] = tool_events.isArray() ? tool_events : Json::Value(Json::arrayValue);
    if (!reasoning.empty())
        assistant["reasoning"] = reasoning;
    else
        assistant["reasoning"] = Json::nullValue;
    assistant["createdAt"] = to_created_at_iso(now);

    messages.append(assistant);
    saveMessages(conversation_id, messages, now);
    return true;
}

std::vector<service::AgentChatMessage> ChatStore::buildAgentMessages(const Json::Value& messages) const
{
    std::vector<service::AgentChatMessage> out;
    if (!messages.isArray())
        return out;

    for (const auto& message : messages)
    {
        if (!message.isMember("role") || !message["role"].isString())
            continue;
        if (!message.isMember("text") || !message["text"].isString())
            continue;

        const std::string role = message["role"].asString();
        if (role != "user" && role != "assistant")
            continue;

        const std::string text = message["text"].asString();
        if (text.empty())
            continue;

        out.push_back({role, text});
    }
    return out;
}

Json::Value ChatStore::defaultSuggestions() const
{
    Json::Value body;
    Json::Value insight_suggestions(Json::arrayValue);
    const char* insight_prompts[] = {
        "오늘 수면 인사이트 알려줘",
        "이번 주 전력 사용량 어때?",
        "오늘 심박수 어때?",
    };
    for (size_t i = 0; i < sizeof(insight_prompts) / sizeof(insight_prompts[0]); ++i)
    {
        Json::Value item;
        item["id"] = "sug_insight_" + std::to_string(i + 1);
        item["label"] = insight_prompts[i];
        item["prompt"] = insight_prompts[i];
        insight_suggestions.append(item);
    }
    body["insightSuggestions"] = insight_suggestions;

    struct SuggestionSeed
    {
        const char* id;
        const char* icon;
        const char* label;
        const char* prompt;
    };

    const SuggestionSeed pool[] = {
        {"sug_welcome_sleep_analysis", "moon", "수면 분석", "어젯밤 수면 점수를 분석해줘"},
        {"sug_welcome_power", "energy", "전력 점검", "지금 집 전체 전력 사용량 알려줘"},
        {"sug_welcome_heart", "heart", "심박 트렌드", "오늘 심박수가 평소와 다른 이유가 뭐야?"},
        {"sug_welcome_iot", "home", "가전 자동화", "취침 전 가전 자동화 설정 도와줘"},
        {"sug_welcome_health", "plan", "헬스 루틴", "이번 주 건강 목표를 세워줘"},
        {"sug_welcome_sleep_env", "sleep", "수면 환경", "더 깊은 수면을 위한 실내 환경 알려줘"},
        {"sug_welcome_temp", "temp", "최적 온도", "수면에 최적인 실내 온도가 몇 도야?"},
        {"sug_welcome_energy", "energy", "에너지 향상", "에너지 점수를 높이는 방법 알려줘"},
    };

    Json::Value suggestion_pool(Json::arrayValue);
    for (const auto& seed : pool)
    {
        Json::Value item;
        item["id"] = seed.id;
        item["icon"] = seed.icon;
        item["label"] = seed.label;
        item["prompt"] = seed.prompt;
        suggestion_pool.append(item);
    }
    body["suggestionPool"] = suggestion_pool;
    return body;
}

} // namespace v1
} // namespace web
WAVE_NAMESPACE_END
