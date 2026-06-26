#pragma once

#include <drogon/HttpClient.h>
#include <json/json.h>
#include <trantor/net/EventLoopThread.h>

#include <cstdint>
#include <functional>
#include <string>

class LmStudioClient
{
public:
    LmStudioClient(const std::string& host, uint16_t port, const std::string& token);

    void listModels() const;
    bool isModelLoaded(const std::string& model) const;
    void loadModel(const std::string& model) const;

    void streamChat(const std::string& model,
                    const std::string& input,
                    float temperature,
                    const std::string& systemPrompt,
                    const std::function<void(const std::string&)>& onDelta,
                    const std::function<void(const std::string&)>& onStatus);

    bool hasConversation() const { return !m_previousResponseId.empty(); }

private:
    void ensureClient() const;
    void applyAuth(const drogon::HttpRequestPtr& req) const;
    Json::Value httpGet(const std::string& path) const;
    Json::Value httpPost(const std::string& path, const Json::Value& body) const;

private:
    std::string m_host;
    uint16_t m_port;
    std::string m_token;
    std::string m_previousResponseId;

    trantor::EventLoopThread m_loopThread;
    mutable drogon::HttpClientPtr m_httpClient;
};
