#include "embedding_client.h"

#include "../core/task_queue.h"

#include <cassert>
#include <cmath>
#include <utility>

#include <drogon/HttpClient.h>
#include <trantor/net/EventLoopThread.h>

WAVE_NAMESPACE_BEGIN
EMBEDDING_NAMESPACE_BEGIN

namespace
{
    constexpr double kHttpTimeoutSeconds = 120.0;

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
}

// ============================================================================
// EmbeddingVector
// ============================================================================

float EmbeddingVector::operator[](size_t index) const
{
    return m_values[index];
}

float EmbeddingVector::at(size_t index) const
{
    return m_values.at(index);
}

float EmbeddingVector::dotProduct(const EmbeddingVector& other) const
{
    const size_t count = std::min(m_values.size(), other.m_values.size());
    float sum = 0.0f;
    for (size_t i = 0; i < count; ++i)
        sum += m_values[i] * other.m_values[i];
    return sum;
}

float EmbeddingVector::similarity(const EmbeddingVector& other) const
{
    const float denom = norm() * other.norm();
    if (denom <= 0.0f)
        return 0.0f;
    return dotProduct(other) / denom;
}

float EmbeddingVector::norm() const
{
    float sum = 0.0f;
    for (float value : m_values)
        sum += value * value;
    return std::sqrt(sum);
}

const float* EmbeddingVector::data() const
{
    return m_values.data();
}

size_t EmbeddingVector::size() const
{
    return m_values.size();
}

bool EmbeddingVector::empty() const
{
    return m_values.empty();
}

// ============================================================================
// EmbeddingClient::Impl
// ============================================================================

struct EmbeddingClient::Impl
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
        std::string& out_response,
        std::string& out_error);

    Result embedRaw(
        const std::vector<std::string>& texts,
        std::vector<std::vector<float>>& out_rows,
        std::string& out_error);
};

bool EmbeddingClient::Impl::parseConfig(const json& config, std::string& out_error)
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

void EmbeddingClient::Impl::ensureClient()
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

void EmbeddingClient::Impl::applyAuth(const drogon::HttpRequestPtr& req) const
{
    if (!apiKey.empty())
        req->addHeader("Authorization", "Bearer " + apiKey);
}

Result EmbeddingClient::Impl::sendRequest(
    drogon::HttpMethod method,
    std::string_view path,
    std::string_view body,
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

Result EmbeddingClient::Impl::embedRaw(
    const std::vector<std::string>& texts,
    std::vector<std::vector<float>>& out_rows,
    std::string& out_error)
{
    if (!initialized)
        return ERROR_NOT_INITIALIZED;

    if (texts.empty())
    {
        out_rows.clear();
        return SUCCESS;
    }

    json request;
    request["model"] = model;
    request["input"] = texts;

    std::string response;
    const Result request_result = sendRequest(
        drogon::Post,
        "/api/embed",
        request.dump(),
        response,
        out_error);
    if (request_result != SUCCESS)
        return request_result;

    json payload;
    try
    {
        payload = json::parse(response);
    }
    catch (const json::exception& e)
    {
        out_error = std::string("response parse error: ") + e.what();
        return ERROR_PARSE;
    }

    if (!payload.contains("embeddings") || !payload["embeddings"].is_array())
    {
        out_error = "response missing 'embeddings' array";
        return ERROR_PARSE;
    }

    const json& rows = payload["embeddings"];
    if (rows.size() != texts.size())
    {
        out_error = "embedding count mismatch: expected " +
            std::to_string(texts.size()) + ", got " + std::to_string(rows.size());
        return ERROR_PARSE;
    }

    out_rows.clear();
    out_rows.reserve(rows.size());
    for (const json& row : rows)
    {
        if (!row.is_array())
        {
            out_error = "embedding row is not an array";
            return ERROR_PARSE;
        }
        out_rows.push_back(row.get<std::vector<float>>());
    }

    return SUCCESS;
}

// ============================================================================
// EmbeddingClient
// ============================================================================

EmbeddingClient::EmbeddingClient() = default;

EmbeddingClient::~EmbeddingClient()
{
    shutdown();
}

Result EmbeddingClient::init(const json& config, std::string& out_error)
{
    shutdown();
    m_impl = std::make_unique<Impl>();
    if (!m_impl->parseConfig(config, out_error))
        return ERROR_INVALID_CONFIG;

    m_impl->ensureClient();
    return SUCCESS;
}

void EmbeddingClient::shutdown()
{
    if (m_impl)
        m_impl->httpClient.reset();
    m_impl.reset();
}

Protocol EmbeddingClient::getProtocol() const
{
    assert(m_impl);
    return m_impl->protocol;
}

std::string_view EmbeddingClient::getHost() const
{
    assert(m_impl);
    return m_impl->baseUrl;
}

std::string_view EmbeddingClient::getModel() const
{
    assert(m_impl);
    return m_impl->model;
}

std::string_view EmbeddingClient::getAPIKey() const
{
    assert(m_impl);
    return m_impl->apiKey;
}

Result EmbeddingClient::ensureModelLoaded()
{
    if (!m_impl || !m_impl->initialized)
        return ERROR_NOT_INITIALIZED;

    std::string response;
    std::string error;
    const Result tags_result = m_impl->sendRequest(
        drogon::Get,
        "/api/tags",
        {},
        response,
        error);
    if (tags_result == SUCCESS)
        return SUCCESS;

    json warmup;
    warmup["model"] = m_impl->model;
    warmup["input"] = "";
    const Result warmup_result = m_impl->sendRequest(
        drogon::Post,
        "/api/embed",
        warmup.dump(),
        response,
        error);
    return warmup_result == SUCCESS ? SUCCESS : ERROR_MODEL;
}

std::future<Result> EmbeddingClient::ensureModelLoadedAsync()
{
    return TaskQueue::enqueueAsync([this]() { return ensureModelLoaded(); });
}

Result EmbeddingClient::embed(std::string_view text, EmbeddingVector& out_embedding)
{
    if (!m_impl)
        return ERROR_NOT_INITIALIZED;

    std::vector<std::string> texts { std::string(text) };
    std::vector<std::vector<float>> rows;
    std::string error;
    const Result result = m_impl->embedRaw(texts, rows, error);
    if (result != SUCCESS)
        return result;

    if (rows.empty())
        return ERROR_PARSE;

    out_embedding.m_values = std::move(rows.front());
    return SUCCESS;
}

Result EmbeddingClient::embedBatch(
    const std::vector<std::string_view>& texts,
    std::vector<EmbeddingVector>& out_embeddings)
{
    if (!m_impl)
        return ERROR_NOT_INITIALIZED;

    std::vector<std::string> owned;
    owned.reserve(texts.size());
    for (std::string_view text : texts)
        owned.emplace_back(text);

    std::vector<std::vector<float>> rows;
    std::string error;
    const Result result = m_impl->embedRaw(owned, rows, error);
    if (result != SUCCESS)
        return result;

    out_embeddings.clear();
    out_embeddings.resize(rows.size());
    for (size_t i = 0; i < rows.size(); ++i)
        out_embeddings[i].m_values = std::move(rows[i]);

    return SUCCESS;
}

std::future<Result> EmbeddingClient::embedAsync(const std::string& text, EmbeddingVector& out_embedding)
{
    std::string owned = text;
    return TaskQueue::enqueueAsync(
        [this, owned = std::move(owned), &out_embedding]() {
            return embed(owned, out_embedding);
        });
}

std::future<Result> EmbeddingClient::embedBatchAsync(
    const std::vector<std::string>& texts,
    std::vector<EmbeddingVector>& out_embeddings)
{
    std::vector<std::string> owned = texts;
    return TaskQueue::enqueueAsync(
        [this, owned = std::move(owned), &out_embeddings]() {
            std::vector<std::string_view> views;
            views.reserve(owned.size());
            for (const std::string& text : owned)
                views.emplace_back(text);
            return embedBatch(views, out_embeddings);
        });
}

EMBEDDING_NAMESPACE_END
WAVE_NAMESPACE_END
