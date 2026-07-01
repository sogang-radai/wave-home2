#pragma once

#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "../core/coredefs.h"
#include "../core/json.h"

#define LLM_NAMESPACE_BEGIN namespace llm {
#define LLM_NAMESPACE_END }

WAVE_NAMESPACE_BEGIN
LLM_NAMESPACE_BEGIN

// ============================================================================
// Content
// ============================================================================

class Content
{
    friend class Client;
    friend class Chat;

public:
    enum class Kind
    {
        Text,
        Image,
    };

    virtual ~Content() = default;

    virtual Kind kind() const = 0;
    virtual std::string_view content() const = 0;

protected:
    std::string m_content;
};

class Image :
    public Content
{
public:
    enum Detail
    {
        Low,
        Medium,
        High,
    };

    static std::shared_ptr<Image> fromBase64(std::string_view base64, Detail detail = Medium);
    static std::shared_ptr<Image> fromFile(std::string_view path, Detail detail = Medium);
    static std::shared_ptr<Image> fromUrl(std::string_view url, Detail detail = Medium);
    ~Image() override = default;

    Kind kind() const override;
    Detail detailTag() const;

    std::string_view content() const override;

private:
    Image(Detail detail, std::string payload);

    Detail m_detail;
};

class Text :
    public Content
{
    friend class Client;
    friend class Chat;

    Text();

public:
    static std::shared_ptr<Text> create(std::string_view message);
    ~Text() override = default;

    Kind kind() const override;
    std::string_view content() const override;

private:
    std::string_view m_message;
    std::string_view m_thinking;
    std::string_view m_answer;
};

// ============================================================================
// Message
// ============================================================================

class Message
{
    friend class Chat;

public:
    using ContentPtr = std::shared_ptr<Content>;

    enum Role
    {
        System,
        User,
        Assistant,
    };

    static std::shared_ptr<Message> create(Role role);
    static std::shared_ptr<Message> create(Role role, ContentPtr content);
    ~Message() = default;

    Role role() const;

    void addContent(ContentPtr content);
    size_t size() const;

    auto begin() const { return m_contents.cbegin(); }
    auto end() const { return m_contents.cend(); }

    void clear();

private:
    Role m_role = User;
    std::vector<ContentPtr> m_contents;
};

// ============================================================================
// Chat
// ============================================================================

class Chat
{
public:
    using ContentPtr = std::shared_ptr<Content>;
    using MessagePtr = std::shared_ptr<Message>;

    struct Options
    {
        float temperature = 0.7f;
        int32_t maxTokens = 0;
        float topP = -1.0f;
        float frequencyPenalty = 0.0f;
        float presencePenalty = 0.0f;
        int32_t seed = -1;
        std::vector<std::string> stop;
        size_t maxContextMessages = 0;
    };

    Chat();
    ~Chat() = default;

    json archive() const;
    bool restore(const json& archive);

    void addMessage(MessagePtr message);

    size_t size() const;

    auto begin() const { return m_messages.cbegin(); }
    auto end() const { return m_messages.cend(); }

    const Options& options() const { return m_options; }
    void setOptions(const Options& options) { m_options = options; }

    void truncate(size_t max_message_count);
    void clear();

private:
    Options m_options {};
    std::vector<MessagePtr> m_messages;
};

// ============================================================================
// Client
// ============================================================================

enum Result
{
    SUCCESS = 0,
    ERROR_NOT_INITIALIZED,
    ERROR_INVALID_CONFIG,
    ERROR_INVALID_PROTOCOL,
    ERROR_NETWORK,
    ERROR_HTTP,
    ERROR_PARSE,
    ERROR_MODEL,
    ERROR_IO,
};

enum Protocol
{
    PROTOCOL_OPENAI
};

class Client
{
public:
    Client();
    ~Client();

    Result init(const json& config, std::string& out_error);
    void shutdown();

    Protocol getProtocol() const;
    std::string_view getHost() const;
    std::string_view getModel() const;
    std::string_view getAPIKey() const;

    Result ensureModelLoaded();
    std::future<Result> ensureModelLoadedAsync();

    Result chat(Chat& chat);
    std::future<Result> chatAsync(Chat& chat);

    Result streamChat(
        Chat& chat,
        const std::function<void(const std::string&)>& on_stream,
        const std::function<void(const std::string&)>& on_thinking = {});

    std::future<Result> streamChatAsync(
        Chat& chat,
        const std::function<void(const std::string&)>& on_stream,
        const std::function<void(const std::string&)>& on_thinking = {});

private:
    static std::string_view textForApi(const Text& text);
    static std::shared_ptr<Text> makeAssistantText(std::string thinking, std::string answer);

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

LLM_NAMESPACE_END
WAVE_NAMESPACE_END
