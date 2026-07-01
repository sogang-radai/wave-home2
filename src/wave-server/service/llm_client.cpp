#include "llm_client.h"

#include "../core/task_queue.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <stb_image.h>

#include <drogon/HttpClient.h>
#include <trantor/net/EventLoopThread.h>

WAVE_NAMESPACE_BEGIN
LLM_NAMESPACE_BEGIN

namespace
{
    constexpr double kHttpTimeoutSeconds = 300.0;

    std::string role_to_string(Message::Role role)
    {
        switch (role)
        {
        case Message::Role::System:
            return "system";
        case Message::Role::User:
            return "user";
        case Message::Role::Assistant:
            return "assistant";
        }
        return "user";
    }

    Message::Role role_from_string(std::string_view role)
    {
        if (role == "system")
            return Message::Role::System;
        if (role == "assistant")
            return Message::Role::Assistant;
        return Message::Role::User;
    }

    std::string_view image_detail_tag(Image::Detail detail)
    {
        switch (detail)
        {
        case Image::Detail::Low:
            return "low";
        case Image::Detail::High:
            return "high";
        case Image::Detail::Medium:
        default:
            return "auto";
        }
    }

    void append_json_string(std::string& out, std::string_view value)
    {
        out.push_back('"');
        for (char ch : value)
        {
            switch (ch)
            {
            case '"':
                out.append("\\\"");
                break;
            case '\\':
                out.append("\\\\");
                break;
            case '\b':
                out.append("\\b");
                break;
            case '\f':
                out.append("\\f");
                break;
            case '\n':
                out.append("\\n");
                break;
            case '\r':
                out.append("\\r");
                break;
            case '\t':
                out.append("\\t");
                break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20)
                {
                    char buffer[7];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", ch);
                    out.append(buffer);
                }
                else
                {
                    out.push_back(ch);
                }
                break;
            }
        }
        out.push_back('"');
    }

    std::string encode_base64(const uint8_t* data, size_t size)
    {
        static constexpr char kAlphabet[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

        std::string out;
        out.reserve(((size + 2) / 3) * 4);

        size_t i = 0;
        while (i + 2 < size)
        {
            const uint32_t triple =
                (static_cast<uint32_t>(data[i]) << 16) |
                (static_cast<uint32_t>(data[i + 1]) << 8) |
                static_cast<uint32_t>(data[i + 2]);
            out.push_back(kAlphabet[(triple >> 18) & 0x3F]);
            out.push_back(kAlphabet[(triple >> 12) & 0x3F]);
            out.push_back(kAlphabet[(triple >> 6) & 0x3F]);
            out.push_back(kAlphabet[triple & 0x3F]);
            i += 3;
        }

        if (i < size)
        {
            const uint32_t triple = static_cast<uint32_t>(data[i]) << 16;
            out.push_back(kAlphabet[(triple >> 18) & 0x3F]);
            if (i + 1 < size)
            {
                const uint32_t pair =
                    (static_cast<uint32_t>(data[i]) << 16) |
                    (static_cast<uint32_t>(data[i + 1]) << 8);
                out.push_back(kAlphabet[(pair >> 12) & 0x3F]);
                out.push_back(kAlphabet[(pair >> 6) & 0x3F]);
                out.push_back('=');
            }
            else
            {
                out.push_back(kAlphabet[(triple >> 12) & 0x3F]);
                out.push_back('=');
                out.push_back('=');
            }
        }

        return out;
    }

    std::string read_file_bytes(std::string_view path, std::string& out_error)
    {
        std::ifstream file(std::string(path), std::ios::binary);
        if (!file.is_open())
        {
            out_error = "Failed to open image file: " + std::string(path);
            return {};
        }

        return std::string(
            std::istreambuf_iterator<char>(file),
            std::istreambuf_iterator<char>());
    }

    std::string detect_image_mime(const uint8_t* data, int size)
    {
        if (size >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF)
            return "image/jpeg";
        if (size >= 8 && std::memcmp(data, "\211PNG\r\n\032\n", 8) == 0)
            return "image/png";
        if (size >= 6 &&
            (std::memcmp(data, "GIF87a", 6) == 0 || std::memcmp(data, "GIF89a", 6) == 0))
            return "image/gif";
        if (size >= 12 &&
            std::memcmp(data, "RIFF", 4) == 0 &&
            std::memcmp(data + 8, "WEBP", 4) == 0)
            return "image/webp";
        return "image/jpeg";
    }

    struct ParsedHost
    {
        std::string baseUrl;
        std::string hostHeader;
    };

    bool parse_host_config(const std::string& host_value, ParsedHost& out, std::string& out_error)
    {
        std::string value = host_value;
        if (value.empty())
        {
            out_error = "host must not be empty";
            return false;
        }

        if (value.find("://") == std::string::npos)
            value = "http://" + value;

        const auto scheme_end = value.find("://");
        const auto host_start = scheme_end + 3;
        const auto path_start = value.find('/', host_start);

        std::string authority = path_start == std::string::npos
            ? value.substr(host_start)
            : value.substr(host_start, path_start - host_start);

        out.baseUrl = path_start == std::string::npos
            ? value
            : value.substr(0, path_start);

        out.hostHeader = authority;
        return true;
    }

    bool parse_sse_block(const std::string& block, std::string& out_data)
    {
        out_data.clear();
        std::istringstream stream(block);
        std::string line;
        while (std::getline(stream, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.rfind("data:", 0) != 0)
                continue;

            std::string data = line.substr(5);
            if (!data.empty() && data.front() == ' ')
                data.erase(0, 1);
            if (!out_data.empty())
                out_data.push_back('\n');
            out_data += data;
        }
        return !out_data.empty();
    }

    std::string extract_openai_content(const json& payload)
    {
        if (!payload.contains("choices") || !payload["choices"].is_array() || payload["choices"].empty())
            return {};

        const json& choice = payload["choices"][0];
        if (choice.contains("message") && choice["message"].contains("content"))
        {
            const json& content = choice["message"]["content"];
            if (content.is_string())
                return content.get<std::string>();
        }
        if (choice.contains("text") && choice["text"].is_string())
            return choice["text"].get<std::string>();
        return {};
    }

    std::string extract_json_string_field(const json& object, std::initializer_list<const char*> keys)
    {
        for (const char* key : keys)
        {
            if (!object.contains(key) || !object[key].is_string())
                continue;

            const std::string value = object[key].get<std::string>();
            if (!value.empty())
                return value;
        }
        return {};
    }

    std::string extract_openai_thinking_content(const json& payload)
    {
        if (!payload.contains("choices") || !payload["choices"].is_array() || payload["choices"].empty())
            return {};

        const json& choice = payload["choices"][0];
        if (choice.contains("message") && choice["message"].is_object())
            return extract_json_string_field(choice["message"], {"reasoning_content", "reasoning", "thinking"});

        return {};
    }

    std::string extract_openai_delta(const json& payload)
    {
        if (!payload.contains("choices") || !payload["choices"].is_array() || payload["choices"].empty())
            return {};

        const json& choice = payload["choices"][0];
        if (choice.contains("delta") &&
            choice["delta"].contains("content") &&
            choice["delta"]["content"].is_string())
        {
            return choice["delta"]["content"].get<std::string>();
        }
        return {};
    }

    std::string extract_openai_thinking_delta(const json& payload)
    {
        if (!payload.contains("choices") || !payload["choices"].is_array() || payload["choices"].empty())
            return {};

        const json& choice = payload["choices"][0];
        if (!choice.contains("delta") || !choice["delta"].is_object())
            return {};

        return extract_json_string_field(choice["delta"], {"reasoning_content", "reasoning", "thinking"});
    }

    struct ParsedEndpoint
    {
        std::string host;
        uint16_t port = 80;
        bool useSsl = false;
    };

    bool parse_endpoint(
        const std::string& base_url,
        const std::string& host_header,
        ParsedEndpoint& out,
        std::string& out_error)
    {
        out.useSsl = base_url.rfind("https://", 0) == 0;
        out.port = out.useSsl ? 443 : 80;

        std::string authority = host_header;
        if (authority.empty())
        {
            out_error = "host header must not be empty";
            return false;
        }

        if (!authority.empty() && authority.front() == '[')
        {
            const size_t end_bracket = authority.find(']');
            if (end_bracket == std::string::npos)
            {
                out_error = "invalid IPv6 host header";
                return false;
            }
            out.host = authority.substr(1, end_bracket - 1);
            if (end_bracket + 1 < authority.size() && authority[end_bracket + 1] == ':')
                out.port = static_cast<uint16_t>(std::stoi(authority.substr(end_bracket + 2)));
        }
        else
        {
            const size_t colon = authority.rfind(':');
            if (colon != std::string::npos && colon + 1 < authority.size())
            {
                bool port_is_numeric = true;
                for (size_t i = colon + 1; i < authority.size(); ++i)
                {
                    if (!std::isdigit(static_cast<unsigned char>(authority[i])))
                    {
                        port_is_numeric = false;
                        break;
                    }
                }
                if (port_is_numeric)
                {
                    out.host = authority.substr(0, colon);
                    out.port = static_cast<uint16_t>(std::stoi(authority.substr(colon + 1)));
                }
                else
                {
                    out.host = authority;
                }
            }
            else
            {
                out.host = authority;
            }
        }

        if (out.host.empty())
        {
            out_error = "host must not be empty";
            return false;
        }
        return true;
    }

    class ChunkedStreamDecoder
    {
    public:
        void feed(std::string_view input, const std::function<void(std::string_view decoded)>& on_decoded)
        {
            m_buffer.append(input);
            while (true)
            {
                if (m_done)
                    return;

                if (m_state == State::ChunkSize)
                {
                    const size_t line_end = m_buffer.find("\r\n");
                    if (line_end == std::string::npos)
                        return;

                    const std::string size_line = m_buffer.substr(0, line_end);
                    m_buffer.erase(0, line_end + 2);
                    m_chunk_size = std::stoul(size_line, nullptr, 16);
                    if (m_chunk_size == 0)
                    {
                        m_done = true;
                        return;
                    }
                    m_state = State::ChunkData;
                }

                if (m_state == State::ChunkData)
                {
                    if (m_buffer.size() < m_chunk_size + 2)
                        return;

                    on_decoded(std::string_view(m_buffer.data(), m_chunk_size));
                    m_buffer.erase(0, m_chunk_size + 2);
                    m_chunk_size = 0;
                    m_state = State::ChunkSize;
                }
            }
        }

        bool done() const
        {
            return m_done;
        }

    private:
        enum class State
        {
            ChunkSize,
            ChunkData,
        };

        State m_state = State::ChunkSize;
        size_t m_chunk_size = 0;
        std::string m_buffer;
        bool m_done = false;
    };

    class SseStreamAccumulator
    {
    public:
        void feed(std::string_view text, const std::function<void(const std::string& data)>& on_event)
        {
            m_buffer.append(text);
            size_t block_end = 0;
            while ((block_end = m_buffer.find("\n\n")) != std::string::npos)
            {
                const std::string block = m_buffer.substr(0, block_end);
                m_buffer.erase(0, block_end + 2);

                std::string data;
                if (parse_sse_block(block, data))
                    on_event(data);
            }
        }

    private:
        std::string m_buffer;
    };

    void handle_stream_data(
        const std::string& data,
        std::string& thinking_text,
        std::string& assistant_text,
        const std::function<void(const std::string&)>& on_stream,
        const std::function<void(const std::string&)>& on_thinking)
    {
        if (data == "[DONE]")
            return;

        json payload;
        try
        {
            payload = json::parse(data);
        }
        catch (const json::exception&)
        {
            return;
        }

        const std::string thinking_delta = extract_openai_thinking_delta(payload);
        if (!thinking_delta.empty())
        {
            thinking_text.append(thinking_delta);
            if (on_thinking)
                on_thinking(thinking_delta);
        }

        const std::string delta = extract_openai_delta(payload);
        if (delta.empty())
            return;

        assistant_text.append(delta);
        if (on_stream)
            on_stream(delta);
    }

    constexpr char kTextRegionSeparator = '\x1E';

    bool http_status_is_ok(std::string_view headers)
    {
        const size_t line_end = headers.find("\r\n");
        if (line_end == std::string::npos)
            return false;

        const std::string status_line(headers.substr(0, line_end));
        return status_line.find(" 200 ") != std::string::npos ||
               status_line.find(" 201 ") != std::string::npos;
    }
}

Image::Image(Detail detail, std::string payload) :
    m_detail(detail)
{
    m_content = std::move(payload);
}

std::shared_ptr<Image> Image::fromBase64(std::string_view base64, Detail detail)
{
    std::string payload;
    if (base64.rfind("data:", 0) == 0)
        payload.assign(base64);
    else
        payload = "data:image/jpeg;base64," + std::string(base64);
    return std::shared_ptr<Image>(new Image(detail, std::move(payload)));
}

std::shared_ptr<Image> Image::fromFile(std::string_view path, Detail detail)
{
    std::string error;
    const std::string bytes = read_file_bytes(path, error);
    if (bytes.empty() && !error.empty())
        throw std::runtime_error(error);

    const auto* data = reinterpret_cast<const uint8_t*>(bytes.data());
    int width = 0;
    int height = 0;
    int channels = 0;
    if (!stbi_info_from_memory(data, static_cast<int>(bytes.size()), &width, &height, &channels))
        throw std::runtime_error("Unsupported or corrupt image file: " + std::string(path));

    const std::string mime = detect_image_mime(data, static_cast<int>(bytes.size()));
    std::string payload = "data:" + mime + ";base64," + encode_base64(data, bytes.size());
    return std::shared_ptr<Image>(new Image(detail, std::move(payload)));
}

std::shared_ptr<Image> Image::fromUrl(std::string_view url, Detail detail)
{
    return std::shared_ptr<Image>(new Image(detail, std::string(url)));
}

Content::Kind Image::kind() const
{
    return Content::Kind::Image;
}

std::string_view Image::content() const
{
    return m_content;
}

Image::Detail Image::detailTag() const
{
    return m_detail;
}

Text::Text()
{
}

std::shared_ptr<Text> Text::create(std::string_view message)
{
    auto text = std::shared_ptr<Text>(new Text());
    text->m_content.assign(message);
    text->m_message = text->m_content;
    text->m_thinking = {};
    text->m_answer = text->m_content;
    return text;
}

Content::Kind Text::kind() const
{
    return Content::Kind::Text;
}

std::string_view Text::content() const
{
    return m_message;
}

std::string_view Client::textForApi(const Text& text)
{
    return !text.m_answer.empty() ? text.m_answer : text.m_message;
}

std::shared_ptr<Text> Client::makeAssistantText(std::string thinking, std::string answer)
{
    auto text = std::shared_ptr<Text>(new Text());
    if (!thinking.empty())
    {
        text->m_content.reserve(thinking.size() + 1 + answer.size());
        text->m_content.assign(thinking);
        text->m_content.push_back(kTextRegionSeparator);
        text->m_content.append(answer);
        text->m_thinking = std::string_view(text->m_content.data(), thinking.size());
        text->m_answer = std::string_view(
            text->m_content.data() + thinking.size() + 1,
            answer.size());
    }
    else
    {
        text->m_content = std::move(answer);
        text->m_thinking = {};
        text->m_answer = text->m_content;
    }
    text->m_message = text->m_answer;
    return text;
}

std::shared_ptr<Message> Message::create(Role role)
{
    auto message = std::make_shared<Message>();
    message->m_role = role;
    return message;
}

std::shared_ptr<Message> Message::create(Role role, ContentPtr content)
{
    auto message = create(role);
    if (content)
        message->m_contents.push_back(std::move(content));
    return message;
}

Message::Role Message::role() const
{
    return m_role;
}

void Message::addContent(ContentPtr content)
{
    if (content)
        m_contents.push_back(std::move(content));
}

size_t Message::size() const
{
    return m_contents.size();
}

void Message::clear()
{
    m_contents.clear();
}

Chat::Chat()
{
}

json Chat::archive() const
{
    json archive = json::object();
    archive["temperature"] = m_options.temperature;
    if (m_options.maxTokens > 0)
        archive["maxTokens"] = m_options.maxTokens;
    if (m_options.topP >= 0.0f)
        archive["topP"] = m_options.topP;
    if (m_options.frequencyPenalty != 0.0f)
        archive["frequencyPenalty"] = m_options.frequencyPenalty;
    if (m_options.presencePenalty != 0.0f)
        archive["presencePenalty"] = m_options.presencePenalty;
    if (m_options.seed >= 0)
        archive["seed"] = m_options.seed;
    if (!m_options.stop.empty())
        archive["stop"] = m_options.stop;
    if (m_options.maxContextMessages > 0)
        archive["maxContextMessages"] = m_options.maxContextMessages;

    json messages = json::array();
    for (const auto& message : m_messages)
    {
        if (!message)
            continue;

        json entry = json::object();
        entry["role"] = role_to_string(message->role());

        if (message->size() == 1 && (*message->begin())->kind() == Content::Kind::Text)
        {
            const auto& text = static_cast<const Text&>(**message->begin());
            entry["content"] = std::string(text.m_message);
        }
        else
        {
            json parts = json::array();
            for (const auto& content : *message)
            {
                if (!content)
                    continue;

                if (content->kind() == Content::Kind::Text)
                {
                    const auto& text = static_cast<const Text&>(*content);
                    parts.push_back({
                        {"type", "text"},
                        {"text", std::string(text.m_message)},
                    });
                }
                else if (const auto image = std::dynamic_pointer_cast<Image>(content))
                {
                    parts.push_back({
                        {"type", "image_url"},
                        {"image_url",
                         {
                             {"url", std::string(image->content())},
                             {"detail", std::string(image_detail_tag(image->detailTag()))},
                         }},
                    });
                }
            }
            entry["content"] = std::move(parts);
        }

        messages.push_back(std::move(entry));
    }

    archive["messages"] = std::move(messages);
    return archive;
}

bool Chat::restore(const json& archive)
{
    try
    {
        if (!archive.is_object())
            return false;

        m_messages.clear();
        if (archive.contains("temperature"))
            m_options.temperature = archive["temperature"].get<float>();
        if (archive.contains("maxTokens"))
            m_options.maxTokens = archive["maxTokens"].get<int32_t>();
        if (archive.contains("topP"))
            m_options.topP = archive["topP"].get<float>();
        if (archive.contains("frequencyPenalty"))
            m_options.frequencyPenalty = archive["frequencyPenalty"].get<float>();
        if (archive.contains("presencePenalty"))
            m_options.presencePenalty = archive["presencePenalty"].get<float>();
        if (archive.contains("seed"))
            m_options.seed = archive["seed"].get<int32_t>();
        if (archive.contains("stop") && archive["stop"].is_array())
            m_options.stop = archive["stop"].get<std::vector<std::string>>();
        if (archive.contains("maxContextMessages"))
            m_options.maxContextMessages = archive["maxContextMessages"].get<size_t>();

        if (!archive.contains("messages") || !archive["messages"].is_array())
            return false;

        for (const auto& entry : archive["messages"])
        {
            if (!entry.is_object() || !entry.contains("role"))
                continue;

            auto message = Message::create(role_from_string(entry["role"].get<std::string>()));
            const json& content = entry["content"];
            if (content.is_string())
            {
                message->addContent(Text::create(content.get<std::string>()));
            }
            else if (content.is_array())
            {
                for (const auto& part : content)
                {
                    if (!part.is_object() || !part.contains("type"))
                        continue;

                    const std::string type = part["type"].get<std::string>();
                    if (type == "text" && part.contains("text"))
                        message->addContent(Text::create(part["text"].get<std::string>()));
                    else if (type == "image_url" && part.contains("image_url"))
                    {
                        const auto& image_url = part["image_url"];
                        if (!image_url.contains("url"))
                            continue;

                        Image::Detail detail = Image::Detail::Medium;
                        if (image_url.contains("detail"))
                        {
                            const std::string detail_tag = image_url["detail"].get<std::string>();
                            if (detail_tag == "low")
                                detail = Image::Detail::Low;
                            else if (detail_tag == "high")
                                detail = Image::Detail::High;
                        }
                        message->addContent(Image::fromUrl(image_url["url"].get<std::string>(), detail));
                    }
                }
            }

            if (message->size() > 0)
                m_messages.push_back(std::move(message));
        }

        return true;
    }
    catch (const json::exception&)
    {
        return false;
    }
}

void Chat::addMessage(MessagePtr message)
{
    if (message)
        m_messages.push_back(std::move(message));
}

size_t Chat::size() const
{
    return m_messages.size();
}

void Chat::truncate(size_t max_message_count)
{
    if (m_messages.size() <= max_message_count)
        return;
    m_messages.erase(
        m_messages.begin(),
        m_messages.end() - static_cast<ptrdiff_t>(max_message_count));
}

void Chat::clear()
{
    m_messages.clear();
}

struct Client::Impl
{
    Protocol protocol = PROTOCOL_OPENAI;
    std::string baseUrl;
    std::string hostHeader;
    std::string model;
    std::string apiKey;
    bool initialized = false;
    bool loopStarted = false;

    trantor::EventLoopThread loopThread;
    drogon::HttpClientPtr httpClient;

    bool parseConfig(const json& config, std::string& out_error);
    void ensureClient();

    void applyAuth(const drogon::HttpRequestPtr& req) const;
    Result sendRequest(
        drogon::HttpMethod method,
        std::string_view path,
        std::string_view body,
        bool acceptEventStream,
        std::string& out_response,
        std::string& out_error);

    Result sendStreamRequest(
        std::string_view path,
        std::string_view body,
        Chat& chat,
        const std::function<void(const std::string&)>& on_stream,
        const std::function<void(const std::string&)>& on_thinking,
        std::string& out_error);

    void appendMessageContent(std::string& out, const Content& content) const;
    void appendMessagesJson(std::string& out, const Chat& chat) const;
    void appendOptionsJson(std::string& out, const Chat::Options& options) const;
    std::string buildChatRequestBody(const Chat& chat, bool stream) const;

    Result parseChatResponse(
        Chat& chat,
        const std::string& response_body,
        const std::function<void(const std::string&)>& on_thinking,
        std::string& out_error) const;
    Result parseStreamResponse(
        Chat& chat,
        const std::string& response_body,
        const std::function<void(const std::string&)>& on_stream,
        const std::function<void(const std::string&)>& on_thinking,
        std::string& out_error) const;

    Result chatInternal(
        Chat& chat,
        bool stream,
        const std::function<void(const std::string&)>& on_stream,
        const std::function<void(const std::string&)>& on_thinking);
};

bool Client::Impl::parseConfig(const json& config, std::string& out_error)
{
    try
    {
        if (!config.is_object())
        {
            out_error = "model config must be a JSON object";
            return false;
        }

        for (const char* key : {"protocol", "host", "model"})
        {
            if (!config.contains(key) || !config[key].is_string())
            {
                out_error = std::string("model config missing string field '") + key + "'";
                return false;
            }
        }

        const std::string protocol_name = config["protocol"].get<std::string>();
        if (protocol_name == "openai-ollama" || protocol_name == "openai")
            protocol = PROTOCOL_OPENAI;
        else
        {
            out_error = "unsupported protocol: " + protocol_name;
            return false;
        }

        ParsedHost parsed {};
        if (!parse_host_config(config["host"].get<std::string>(), parsed, out_error))
            return false;

        baseUrl = parsed.baseUrl;
        hostHeader = parsed.hostHeader;
        model = config["model"].get<std::string>();
        apiKey = config.value("api-key", "");
        initialized = true;
        return true;
    }
    catch (const json::exception& e)
    {
        out_error = std::string("model config parse error: ") + e.what();
        return false;
    }
}

void Client::Impl::ensureClient()
{
    if (httpClient)
        return;

    if (!loopStarted)
    {
        loopThread.run();
        loopStarted = true;
    }

    httpClient = drogon::HttpClient::newHttpClient(baseUrl, loopThread.getLoop());
}

void Client::Impl::applyAuth(const drogon::HttpRequestPtr& req) const
{
    if (!apiKey.empty())
        req->addHeader("Authorization", "Bearer " + apiKey);
}

Result Client::Impl::sendRequest(
    drogon::HttpMethod method,
    std::string_view path,
    std::string_view body,
    bool acceptEventStream,
    std::string& out_response,
    std::string& out_error)
{
    ensureClient();

    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(method);
    req->setPath(std::string(path));
    if (!hostHeader.empty())
        req->addHeader("Host", hostHeader);
    if (!body.empty())
    {
        req->setBody(std::string(body));
        req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    }
    if (acceptEventStream)
        req->addHeader("Accept", "text/event-stream");
    applyAuth(req);

    const auto [result, resp] = httpClient->sendRequest(req, kHttpTimeoutSeconds);
    if (result != drogon::ReqResult::Ok || !resp)
    {
        out_error = "HTTP request failed: " + drogon::to_string(result);
        return ERROR_NETWORK;
    }

    out_response.assign(resp->getBody());
    if (resp->getStatusCode() != drogon::k200OK)
    {
        out_error = "HTTP status " + std::to_string(resp->getStatusCode()) + ": " + out_response;
        return ERROR_HTTP;
    }

    return SUCCESS;
}

Result Client::Impl::sendStreamRequest(
    std::string_view path,
    std::string_view body,
    Chat& chat,
    const std::function<void(const std::string&)>& on_stream,
    const std::function<void(const std::string&)>& on_thinking,
    std::string& out_error)
{
    if (baseUrl.rfind("https://", 0) == 0)
    {
        out_error = "incremental streaming over HTTPS is not supported";
        return ERROR_INVALID_PROTOCOL;
    }

    ParsedEndpoint endpoint {};
    if (!parse_endpoint(baseUrl, hostHeader, endpoint, out_error))
        return ERROR_INVALID_CONFIG;

    addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* result = nullptr;
    const std::string port_str = std::to_string(endpoint.port);
    const int resolve_rc = getaddrinfo(endpoint.host.c_str(), port_str.c_str(), &hints, &result);
    if (resolve_rc != 0)
    {
        out_error = std::string("DNS resolve failed: ") + gai_strerror(resolve_rc);
        return ERROR_NETWORK;
    }

    int sock = -1;
    for (addrinfo* ptr = result; ptr != nullptr; ptr = ptr->ai_next)
    {
        sock = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
        if (sock < 0)
            continue;
        if (connect(sock, ptr->ai_addr, ptr->ai_addrlen) == 0)
            break;
        close(sock);
        sock = -1;
    }
    freeaddrinfo(result);

    if (sock < 0)
    {
        out_error = "Failed to connect to " + endpoint.host + ":" + port_str;
        return ERROR_NETWORK;
    }

    const std::string body_str(body);
    std::ostringstream request;
    request << "POST " << path << " HTTP/1.1\r\n"
            << "Host: " << hostHeader << "\r\n"
            << "Content-Type: application/json\r\n"
            << "Accept: text/event-stream\r\n"
            << "Connection: close\r\n";
    if (!apiKey.empty())
        request << "Authorization: Bearer " << apiKey << "\r\n";
    request << "Content-Length: " << body_str.size() << "\r\n\r\n"
            << body_str;

    const std::string request_data = request.str();
    size_t sent_total = 0;
    while (sent_total < request_data.size())
    {
        const ssize_t sent = send(
            sock,
            request_data.data() + sent_total,
            request_data.size() - sent_total,
            0);
        if (sent <= 0)
        {
            close(sock);
            out_error = "Failed to send chat request";
            return ERROR_NETWORK;
        }
        sent_total += static_cast<size_t>(sent);
    }

    std::string thinking_text;
    thinking_text.reserve(4096);
    std::string assistant_text;
    assistant_text.reserve(4096);
    std::string raw_buffer;
    bool headers_parsed = false;
    bool use_chunked_decoder = false;
    ChunkedStreamDecoder chunked_decoder;
    SseStreamAccumulator sse_accumulator;

    const auto process_decoded = [&](std::string_view decoded) {
        sse_accumulator.feed(decoded, [&](const std::string& data) {
            handle_stream_data(data, thinking_text, assistant_text, on_stream, on_thinking);
        });
    };

    char read_buffer[4096];
    while (true)
    {
        const ssize_t received = recv(sock, read_buffer, sizeof(read_buffer), 0);
        if (received < 0)
        {
            close(sock);
            out_error = "Failed to read chat response";
            return ERROR_NETWORK;
        }
        if (received == 0)
            break;

        raw_buffer.append(read_buffer, static_cast<size_t>(received));

        if (!headers_parsed)
        {
            const size_t header_end = raw_buffer.find("\r\n\r\n");
            if (header_end == std::string::npos)
                continue;

            const std::string headers = raw_buffer.substr(0, header_end);
            if (!http_status_is_ok(headers))
            {
                const std::string error_body = raw_buffer.substr(header_end + 4);
                close(sock);
                out_error = "HTTP error: " + error_body;
                return ERROR_HTTP;
            }

            std::string lower_headers = headers;
            std::transform(
                lower_headers.begin(),
                lower_headers.end(),
                lower_headers.begin(),
                [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            use_chunked_decoder = lower_headers.find("transfer-encoding: chunked") != std::string::npos;

            const std::string body_part = raw_buffer.substr(header_end + 4);
            raw_buffer.clear();
            headers_parsed = true;

            if (use_chunked_decoder)
                chunked_decoder.feed(body_part, process_decoded);
            else
                process_decoded(body_part);
            continue;
        }

        if (use_chunked_decoder)
            chunked_decoder.feed(raw_buffer, process_decoded);
        else
            process_decoded(raw_buffer);
        raw_buffer.clear();
    }

    close(sock);

    if (!raw_buffer.empty())
    {
        if (use_chunked_decoder)
            chunked_decoder.feed(raw_buffer, process_decoded);
        else
            process_decoded(raw_buffer);
    }

    if (assistant_text.empty())
    {
        out_error = "stream response did not contain assistant content";
        return ERROR_PARSE;
    }

    chat.addMessage(Message::create(Message::Role::Assistant, Client::makeAssistantText(thinking_text, assistant_text)));
    return SUCCESS;
}

void Client::Impl::appendMessageContent(std::string& out, const Content& content) const
{
    if (content.kind() == Content::Kind::Text)
    {
        const auto& text = static_cast<const Text&>(content);
        out.append("{\"type\":\"text\",\"text\":");
        append_json_string(out, Client::textForApi(text));
        out.push_back('}');
        return;
    }

    const auto& image = static_cast<const Image&>(content);
    out.append("{\"type\":\"image_url\",\"image_url\":{\"url\":");
    append_json_string(out, image.content());
    out.append(",\"detail\":");
    append_json_string(out, image_detail_tag(image.detailTag()));
    out.append("}}");
}

void Client::Impl::appendMessagesJson(std::string& out, const Chat& chat) const
{
    out.append("[");
    bool first_message = true;
    for (const auto& message : chat)
    {
        if (!message || message->size() == 0)
            continue;

        if (!first_message)
            out.push_back(',');
        first_message = false;

        out.append("{\"role\":");
        append_json_string(out, role_to_string(message->role()));
        out.append(",\"content\":");

        if (message->size() == 1 && (*message->begin())->kind() == Content::Kind::Text)
        {
            const auto& text = static_cast<const Text&>(**message->begin());
            append_json_string(out, Client::textForApi(text));
        }
        else
        {
            out.push_back('[');
            bool first_part = true;
            for (const auto& content : *message)
            {
                if (!content)
                    continue;
                if (!first_part)
                    out.push_back(',');
                first_part = false;
                appendMessageContent(out, *content);
            }
            out.push_back(']');
        }

        out.push_back('}');
    }
    out.push_back(']');
}

void Client::Impl::appendOptionsJson(std::string& out, const Chat::Options& options) const
{
    out.append(",\"temperature\":");
    out.append(std::to_string(options.temperature));

    if (options.maxTokens > 0)
    {
        out.append(",\"max_tokens\":");
        out.append(std::to_string(options.maxTokens));
    }

    if (options.topP >= 0.0f)
    {
        out.append(",\"top_p\":");
        out.append(std::to_string(options.topP));
    }

    if (options.frequencyPenalty != 0.0f)
    {
        out.append(",\"frequency_penalty\":");
        out.append(std::to_string(options.frequencyPenalty));
    }

    if (options.presencePenalty != 0.0f)
    {
        out.append(",\"presence_penalty\":");
        out.append(std::to_string(options.presencePenalty));
    }

    if (options.seed >= 0)
    {
        out.append(",\"seed\":");
        out.append(std::to_string(options.seed));
    }

    if (!options.stop.empty())
    {
        out.append(",\"stop\":[");
        bool first_stop = true;
        for (const auto& token : options.stop)
        {
            if (!first_stop)
                out.push_back(',');
            first_stop = false;
            append_json_string(out, token);
        }
        out.push_back(']');
    }
}

std::string Client::Impl::buildChatRequestBody(const Chat& chat, bool stream) const
{
    std::string body;
    body.reserve(4096);
    body.append("{\"model\":");
    append_json_string(body, model);
    body.append(",\"messages\":");
    appendMessagesJson(body, chat);
    appendOptionsJson(body, chat.options());
    body.append(",\"stream\":");
    body.append(stream ? "true" : "false");
    body.push_back('}');
    return body;
}

Result Client::Impl::parseChatResponse(
    Chat& chat,
    const std::string& response_body,
    const std::function<void(const std::string&)>& on_thinking,
    std::string& out_error) const
{
    json payload;
    try
    {
        payload = json::parse(response_body);
    }
    catch (const json::exception& e)
    {
        out_error = std::string("response parse error: ") + e.what();
        return ERROR_PARSE;
    }

    const std::string thinking = extract_openai_thinking_content(payload);
    if (!thinking.empty() && on_thinking)
        on_thinking(thinking);

    const std::string content = extract_openai_content(payload);
    if (content.empty())
    {
        out_error = "response missing assistant content";
        return ERROR_PARSE;
    }

    chat.addMessage(Message::create(Message::Role::Assistant, Client::makeAssistantText(thinking, content)));
    return SUCCESS;
}

Result Client::Impl::parseStreamResponse(
    Chat& chat,
    const std::string& response_body,
    const std::function<void(const std::string&)>& on_stream,
    const std::function<void(const std::string&)>& on_thinking,
    std::string& out_error) const
{
    std::string thinking_text;
    thinking_text.reserve(response_body.size() / 4);
    std::string assistant_text;
    assistant_text.reserve(response_body.size() / 4);

    size_t offset = 0;
    while (offset < response_body.size())
    {
        const size_t block_end = response_body.find("\n\n", offset);
        const size_t end = block_end == std::string::npos ? response_body.size() : block_end;
        const std::string block = response_body.substr(offset, end - offset);
        offset = block_end == std::string::npos ? response_body.size() : block_end + 2;

        std::string data;
        if (!parse_sse_block(block, data))
            continue;

        handle_stream_data(data, thinking_text, assistant_text, on_stream, on_thinking);
    }

    if (assistant_text.empty())
    {
        out_error = "stream response did not contain assistant content";
        return ERROR_PARSE;
    }

    chat.addMessage(Message::create(Message::Role::Assistant, Client::makeAssistantText(thinking_text, assistant_text)));
    return SUCCESS;
}

Result Client::Impl::chatInternal(
    Chat& chat,
    bool stream,
    const std::function<void(const std::string&)>& on_stream,
    const std::function<void(const std::string&)>& on_thinking)
{
    if (!initialized)
        return ERROR_NOT_INITIALIZED;

    if (chat.options().maxContextMessages > 0)
        chat.truncate(chat.options().maxContextMessages);

    const std::string body = buildChatRequestBody(chat, stream);
    std::string error;
    if (stream)
    {
        return sendStreamRequest(
            "/v1/chat/completions",
            body,
            chat,
            on_stream,
            on_thinking,
            error);
    }

    std::string response;
    const Result request_result = sendRequest(
        drogon::Post,
        "/v1/chat/completions",
        body,
        false,
        response,
        error);
    if (request_result != SUCCESS)
        return request_result;

    return parseChatResponse(chat, response, on_thinking, error);
}

Client::Client() :
    m_impl(std::make_unique<Impl>())
{
}

Client::~Client()
{
    shutdown();
}

Result Client::init(const json& config, std::string& out_error)
{
    shutdown();
    m_impl = std::make_unique<Impl>();
    if (!m_impl->parseConfig(config, out_error))
        return ERROR_INVALID_CONFIG;

    m_impl->ensureClient();
    return SUCCESS;
}

void Client::shutdown()
{
    if (m_impl)
        m_impl->httpClient.reset();
    m_impl.reset();
    m_impl = std::make_unique<Impl>();
}

Protocol Client::getProtocol() const
{
    assert(m_impl);
    return m_impl->protocol;
}

std::string_view Client::getHost() const
{
    assert(m_impl);
    return m_impl->baseUrl;
}

std::string_view Client::getModel() const
{
    assert(m_impl);
    return m_impl->model;
}

std::string_view Client::getAPIKey() const
{
    assert(m_impl);
    return m_impl->apiKey;
}

Result Client::ensureModelLoaded()
{
    assert(m_impl);
    if (!m_impl->initialized)
        return ERROR_NOT_INITIALIZED;

    std::string response;
    std::string error;
    const Result tags_result = m_impl->sendRequest(
        drogon::Get,
        "/api/tags",
        {},
        false,
        response,
        error);
    if (tags_result == SUCCESS)
        return SUCCESS;

    const json warmup = {
        {"model", m_impl->model},
        {"prompt", ""},
        {"keep_alive", "5m"},
    };
    const Result warmup_result = m_impl->sendRequest(
        drogon::Post,
        "/api/generate",
        warmup.dump(),
        false,
        response,
        error);
    return warmup_result == SUCCESS ? SUCCESS : ERROR_MODEL;
}

std::future<Result> Client::ensureModelLoadedAsync()
{
    return TaskQueue::enqueueAsync([this]() { return ensureModelLoaded(); });
}

Result Client::chat(Chat& chat)
{
    assert(m_impl);
    return m_impl->chatInternal(chat, false, {}, {});
}

std::future<Result> Client::chatAsync(Chat& chat)
{
    return TaskQueue::enqueueAsync([this, &chat]() { return this->chat(chat); });
}

Result Client::streamChat(
    Chat& chat,
    const std::function<void(const std::string&)>& on_stream,
    const std::function<void(const std::string&)>& on_thinking)
{
    assert(m_impl);
    return m_impl->chatInternal(chat, true, on_stream, on_thinking);
}

std::future<Result> Client::streamChatAsync(
    Chat& chat,
    const std::function<void(const std::string&)>& on_stream,
    const std::function<void(const std::string&)>& on_thinking)
{
    return TaskQueue::enqueueAsync(
        [this, &chat, on_stream, on_thinking]() {
            return streamChat(chat, on_stream, on_thinking);
        });
}

LLM_NAMESPACE_END
WAVE_NAMESPACE_END
