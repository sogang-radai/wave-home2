#include "lm_studio_client.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace
{
    constexpr int kHttpTimeoutSeconds = 300;
    
    std::string jsonToString(const Json::Value& value)
    {
        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";
        return Json::writeString(builder, value);
    }
    
    std::string trim(const std::string& value)
    {
        const auto start = value.find_first_not_of(" \t\r\n");
        if (start == std::string::npos)
            return "";
        const auto end = value.find_last_not_of(" \t\r\n");
        return value.substr(start, end - start + 1);
    }
    
    bool parseSseBlock(const std::string& block,
                       std::string& eventType,
                       std::string& data)
    {
        eventType.clear();
        data.clear();
    
        std::istringstream stream(block);
        std::string line;
        while (std::getline(stream, line)) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
        
            if (line.rfind("event:", 0) == 0) {
                eventType = trim(line.substr(6));
            } else if (line.rfind("data:", 0) == 0) {
                if (!data.empty())
                    data.push_back('\n');
                data += line.substr(5);
                if (!data.empty() && data.front() == ' ')
                    data.erase(0, 1);
            }
        }
    
        return !data.empty();
    }
}  // namespace

LmStudioClient::LmStudioClient(const std::string& host, uint16_t port, const std::string& token) :
    m_host(host),
    m_port(port),
    m_token(token)
{
    m_loopThread.run();
    ensureClient();
}

void LmStudioClient::ensureClient() const
{
    if (m_httpClient)
        return;

    const std::string baseUrl = "http://" + m_host + ":" + std::to_string(m_port);
    m_httpClient = drogon::HttpClient::newHttpClient(baseUrl, m_loopThread.getLoop());
}

void LmStudioClient::applyAuth(const drogon::HttpRequestPtr& req) const
{
    if (!m_token.empty())
        req->addHeader("Authorization", "Bearer " + m_token);
}

Json::Value LmStudioClient::httpGet(const std::string& path) const
{
    ensureClient();

    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Get);
    req->setPath(path);
    applyAuth(req);

    const auto [result, resp] =
        m_httpClient->sendRequest(req, kHttpTimeoutSeconds);
    if (result != drogon::ReqResult::Ok || !resp) {
        throw std::runtime_error("HTTP GET failed: " + drogon::to_string(result));
    }
    if (resp->getStatusCode() != drogon::k200OK) {
        throw std::runtime_error("HTTP GET " + path + " returned status " +
                                 std::to_string(resp->getStatusCode()) + ": " +
                                 std::string(resp->getBody()));
    }

    Json::Value json;
    Json::CharReaderBuilder builder;
    std::string errors;
    const std::string body = std::string(resp->getBody());
    std::istringstream stream(body);
    if (!Json::parseFromStream(builder, stream, &json, &errors)) {
        throw std::runtime_error("Failed to parse JSON response: " + errors);
    }
    return json;
}

Json::Value LmStudioClient::httpPost(const std::string& path,
                                     const Json::Value& body) const
{
    ensureClient();

    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    req->setPath(path);
    req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    req->setBody(jsonToString(body));
    applyAuth(req);

    const auto [result, resp] =
        m_httpClient->sendRequest(req, kHttpTimeoutSeconds);
    if (result != drogon::ReqResult::Ok || !resp) {
        throw std::runtime_error("HTTP POST failed: " + drogon::to_string(result));
    }
    if (resp->getStatusCode() != drogon::k200OK) {
        throw std::runtime_error("HTTP POST " + path + " returned status " +
                                 std::to_string(resp->getStatusCode()) + ": " +
                                 std::string(resp->getBody()));
    }

    Json::Value json;
    Json::CharReaderBuilder builder;
    std::string errors;
    const std::string payload = std::string(resp->getBody());
    std::istringstream stream(payload);
    if (!Json::parseFromStream(builder, stream, &json, &errors)) {
        throw std::runtime_error("Failed to parse JSON response: " + errors);
    }
    return json;
}

void LmStudioClient::listModels() const
{
    const Json::Value response = httpGet("/api/v1/models");
    if (!response.isMember("models") || !response["models"].isArray()) {
        throw std::runtime_error("Unexpected models response format");
    }

    for (const auto& model : response["models"]) {
        const std::string key = model.get("key", "").asString();
        const std::string displayName = model.get("display_name", key).asString();
        const std::string type = model.get("type", "").asString();
        const bool loaded = model.isMember("loaded_instances") &&
                            model["loaded_instances"].isArray() &&
                            !model["loaded_instances"].empty();

        std::cout << key;
        if (!displayName.empty() && displayName != key)
            std::cout << " (" << displayName << ")";
        std::cout << " [" << type << "]";
        if (loaded)
            std::cout << " *loaded*";
        std::cout << "\n";
    }
}

bool LmStudioClient::isModelLoaded(const std::string& model) const
{
    const Json::Value response = httpGet("/api/v1/models");
    if (!response.isMember("models") || !response["models"].isArray())
        return false;

    for (const auto& entry : response["models"]) {
        if (entry.get("key", "").asString() != model)
            continue;

        return entry.isMember("loaded_instances") &&
               entry["loaded_instances"].isArray() &&
               !entry["loaded_instances"].empty();
    }
    return false;
}

void LmStudioClient::loadModel(const std::string& model) const
{
    Json::Value body;
    body["model"] = model;
    httpPost("/api/v1/models/load", body);
}

void LmStudioClient::streamChat(const std::string& model,
                                const std::string& input,
                                float temperature,
                                const std::string& systemPrompt,
                                const std::function<void(const std::string&)>& onDelta,
                                const std::function<void(const std::string&)>& onStatus)
{
    Json::Value body;
    body["model"] = model;
    body["input"] = input;
    body["stream"] = true;
    body["temperature"] = temperature;
    body["store"] = true;

    if (!m_previousResponseId.empty())
        body["previous_response_id"] = m_previousResponseId;
    else if (!systemPrompt.empty())
        body["system_prompt"] = systemPrompt;

    const std::string payload = jsonToString(body);

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* result = nullptr;
    const std::string portStr = std::to_string(m_port);
    const int resolveRc = getaddrinfo(m_host.c_str(), portStr.c_str(), &hints, &result);
    if (resolveRc != 0) {
        throw std::runtime_error("DNS resolve failed: " +
                                 std::string(gai_strerror(resolveRc)));
    }

    int sock = -1;
    for (addrinfo* ptr = result; ptr != nullptr; ptr = ptr->ai_next) {
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
        throw std::runtime_error("Failed to connect to " + m_host + ":" + portStr);

    std::ostringstream request;
    request << "POST /api/v1/chat HTTP/1.1\r\n"
            << "Host: " << m_host << ":" << portStr << "\r\n"
            << "Content-Type: application/json\r\n"
            << "Accept: text/event-stream\r\n"
            << "Connection: close\r\n";
    if (!m_token.empty())
        request << "Authorization: Bearer " << m_token << "\r\n";
    request << "Content-Length: " << payload.size() << "\r\n\r\n"
            << payload;

    const std::string requestData = request.str();
    size_t sentTotal = 0;
    while (sentTotal < requestData.size()) {
        const ssize_t sent = send(sock,
                                  requestData.data() + sentTotal,
                                  requestData.size() - sentTotal,
                                  0);
        if (sent <= 0) {
            close(sock);
            throw std::runtime_error("Failed to send chat request");
        }
        sentTotal += static_cast<size_t>(sent);
    }

    std::string responseBuffer;
    char readBuffer[4096];
    bool headersParsed = false;
    std::string sseBuffer;
    bool firstDelta = true;

    while (true) {
        const ssize_t received = recv(sock, readBuffer, sizeof(readBuffer), 0);
        if (received < 0) {
            close(sock);
            throw std::runtime_error("Failed to read chat response");
        }
        if (received == 0)
            break;

        responseBuffer.append(readBuffer, static_cast<size_t>(received));

        if (!headersParsed) {
            const size_t headerEnd = responseBuffer.find("\r\n\r\n");
            if (headerEnd == std::string::npos)
                continue;

            const std::string headers = responseBuffer.substr(0, headerEnd);
            if (headers.find("200") == std::string::npos) {
                const std::string errorBody = responseBuffer.substr(headerEnd + 4);
                close(sock);
                throw std::runtime_error("Chat request failed: " + errorBody);
            }

            sseBuffer = responseBuffer.substr(headerEnd + 4);
            responseBuffer.clear();
            headersParsed = true;
        } else {
            sseBuffer.append(responseBuffer);
            responseBuffer.clear();
        }

        size_t blockEnd = 0;
        while ((blockEnd = sseBuffer.find("\n\n")) != std::string::npos) {
            const std::string block = sseBuffer.substr(0, blockEnd);
            sseBuffer.erase(0, blockEnd + 2);

            std::string eventType;
            std::string data;
            if (!parseSseBlock(block, eventType, data))
                continue;

            Json::Value event;
            Json::CharReaderBuilder builder;
            std::string errors;
            std::istringstream dataStream(data);
            if (!Json::parseFromStream(builder, dataStream, &event, &errors))
                continue;

            const std::string type = event.get("type", eventType).asString();

            if (type == "message.delta" || type == "reasoning.delta") {
                const std::string content = event.get("content", "").asString();
                if (!content.empty() && onDelta) {
                    if (firstDelta) {
                        onDelta(content);
                        firstDelta = false;
                    } else {
                        onDelta(content);
                    }
                }
            } else if (type == "model_load.start") {
                if (onStatus)
                    onStatus("Loading model...");
            } else if (type == "model_load.progress") {
                if (onStatus && event.isMember("progress")) {
                    const int percent =
                        static_cast<int>(event["progress"].asDouble() * 100.0);
                    onStatus("Loading model... " + std::to_string(percent) + "%");
                }
            } else if (type == "prompt_processing.start") {
                if (onStatus)
                    onStatus("Processing prompt...");
            } else if (type == "error") {
                const std::string message =
                    event["error"].get("message", "Unknown error").asString();
                close(sock);
                throw std::runtime_error(message);
            } else if (type == "chat.end") {
                if (event.isMember("result") &&
                    event["result"].isMember("response_id")) {
                    m_previousResponseId =
                        event["result"]["response_id"].asString();
                }
                close(sock);
                return;
            }
        }
    }

    close(sock);
}
