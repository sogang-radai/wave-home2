#pragma once

#include <cstddef>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "../core/json.h"

#define EMBEDDING_NAMESPACE_BEGIN namespace embedding {
#define EMBEDDING_NAMESPACE_END }

WAVE_NAMESPACE_BEGIN
EMBEDDING_NAMESPACE_BEGIN

// ============================================================================
// Types
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
    PROTOCOL_OPENAI,
};

// ============================================================================
// Client
// ============================================================================

class EmbeddingVector
{
    friend class EmbeddingClient;
public:
    EmbeddingVector() = default;
    ~EmbeddingVector() = default;

    float operator[](size_t index) const;
    float at(size_t index) const;

    float dotProduct(const EmbeddingVector& other) const;
    float similarity(const EmbeddingVector& other) const;
    float norm() const;

    const float* data() const;
    size_t size() const;

    auto begin() const { return m_values.cbegin(); }
    auto end() const { return m_values.cend(); }
    
    bool empty() const;

private:
    std::vector<float> m_values;
};

class EmbeddingClient
{
public:
    EmbeddingClient();
    ~EmbeddingClient();

    Result init(const json& config, std::string& out_error);
    void shutdown();

    Protocol getProtocol() const;
    std::string_view getHost() const;
    std::string_view getModel() const;
    std::string_view getAPIKey() const;

    Result ensureModelLoaded();
    std::future<Result> ensureModelLoadedAsync();

    Result embed(std::string_view text, EmbeddingVector& out_embedding);
    Result embedBatch(const std::vector<std::string_view>& texts, std::vector<EmbeddingVector>& out_embeddings);

    std::future<Result> embedAsync(const std::string& text, EmbeddingVector& out_embedding);
    std::future<Result> embedBatchAsync(const std::vector<std::string>& texts, std::vector<EmbeddingVector>& out_embeddings);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

EMBEDDING_NAMESPACE_END
WAVE_NAMESPACE_END
