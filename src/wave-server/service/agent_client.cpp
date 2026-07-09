#include "agent_client.h"

#include <algorithm>
#include <arpa/inet.h>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <netdb.h>
#include <sstream>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

WAVE_NAMESPACE_BEGIN
SERVICE_NAMESPACE_BEGIN

namespace
{
    constexpr double kHttpTimeoutSeconds = 300.0;

    struct ParsedEndpoint
    {
        std::string host;
        uint16_t port = 80;
        bool use_ssl = false;
        std::string path_prefix;
    };

    bool parse_base_url(const std::string& base_url, ParsedEndpoint& out, std::string& out_error)
    {
        std::string value = base_url;
        if (value.empty())
        {
            out_error = "agent base_url is empty";
            return false;
        }
        if (value.find("://") == std::string::npos)
            value = "http://" + value;

        out.use_ssl = value.rfind("https://", 0) == 0;
        out.port = out.use_ssl ? 443 : 80;

        const auto scheme_end = value.find("://");
        const auto host_start = scheme_end + 3;
        const auto path_start = value.find('/', host_start);

        std::string authority = path_start == std::string::npos
            ? value.substr(host_start)
            : value.substr(host_start, path_start - host_start);
        out.path_prefix = path_start == std::string::npos ? "" : value.substr(path_start);
        if (!out.path_prefix.empty() && out.path_prefix.back() == '/')
            out.path_prefix.pop_back();

        if (!authority.empty() && authority.front() == '[')
        {
            const size_t end_bracket = authority.find(']');
            if (end_bracket == std::string::npos)
            {
                out_error = "invalid IPv6 host in agent base_url";
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
            out_error = "agent host is empty";
            return false;
        }
        if (out.use_ssl)
        {
            out_error = "HTTPS agent streaming is not supported yet";
            return false;
        }
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

                if (m_state == State::chunk_size)
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
                    m_state = State::chunk_data;
                }

                if (m_state == State::chunk_data)
                {
                    if (m_buffer.size() < m_chunk_size + 2)
                        return;

                    on_decoded(std::string_view(m_buffer.data(), m_chunk_size));
                    m_buffer.erase(0, m_chunk_size + 2);
                    m_chunk_size = 0;
                    m_state = State::chunk_size;
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
            chunk_size,
            chunk_data,
        };

        State m_state = State::chunk_size;
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

    bool http_status_is_ok(std::string_view headers)
    {
        const size_t line_end = headers.find("\r\n");
        if (line_end == std::string::npos)
            return false;

        const std::string status_line(headers.substr(0, line_end));
        return status_line.find(" 200 ") != std::string::npos ||
               status_line.find(" 201 ") != std::string::npos ||
               status_line.find(" 202 ") != std::string::npos;
    }

    std::string build_request_body(const AgentChatTurnRequest& request, bool stream)
    {
        json body;
        body["chatHistoryId"] = request.chat_history_id;
        body["userId"] = request.user_id;
        body["stream"] = stream;

        json messages = json::array();
        for (const auto& message : request.messages)
        {
            messages.push_back({
                {"role", message.role},
                {"content", message.content},
            });
        }
        body["messages"] = std::move(messages);

        if (!request.now.empty())
            body["context"] = {{"now", request.now}};

        return body.dump();
    }

    AgentClientResult send_http_request(
        const std::string& base_url,
        std::string_view method,
        std::string_view path,
        std::string_view body,
        bool accept_event_stream,
        const std::function<bool(const std::string& response_body, bool is_stream)>& on_response,
        std::string& out_error)
    {
        ParsedEndpoint endpoint {};
        if (!parse_base_url(base_url, endpoint, out_error))
            return AgentClientResult::parse_error;

        addrinfo hints {};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        addrinfo* result = nullptr;
        const std::string port_str = std::to_string(endpoint.port);
        const int resolve_rc = getaddrinfo(endpoint.host.c_str(), port_str.c_str(), &hints, &result);
        if (resolve_rc != 0)
        {
            out_error = std::string("DNS resolve failed: ") + gai_strerror(resolve_rc);
            return AgentClientResult::network_error;
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
            out_error = "Failed to connect to agent at " + endpoint.host + ":" + port_str;
            return AgentClientResult::network_error;
        }

        const std::string body_str(body);
        const std::string host_header = endpoint.port == 80
            ? endpoint.host
            : endpoint.host + ":" + port_str;

        std::ostringstream request;
        request << method << " " << path << " HTTP/1.1\r\n"
                << "Host: " << host_header << "\r\n"
                << "Content-Type: application/json\r\n"
                << "Connection: close\r\n";
        if (accept_event_stream)
            request << "Accept: text/event-stream\r\n";
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
                out_error = "Failed to send agent request";
                return AgentClientResult::network_error;
            }
            sent_total += static_cast<size_t>(sent);
        }

        std::string raw_buffer;
        bool headers_parsed = false;
        bool use_chunked_decoder = false;
        ChunkedStreamDecoder chunked_decoder;
        SseStreamAccumulator sse_accumulator;
        std::string response_body;
        bool cancelled = false;

        const auto process_decoded = [&](std::string_view decoded) {
            if (accept_event_stream)
            {
                sse_accumulator.feed(decoded, [&](const std::string& data) {
                    if (cancelled)
                        return;
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

                    if (!on_response(data, true))
                        cancelled = true;
                });
            }
            else
            {
                response_body.append(decoded);
            }
        };

        char read_buffer[4096];
        while (true)
        {
            const ssize_t received = recv(sock, read_buffer, sizeof(read_buffer), 0);
            if (received < 0)
            {
                close(sock);
                out_error = "Failed to read agent response";
                return AgentClientResult::network_error;
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
                    out_error = "Agent HTTP error: " + error_body;
                    return AgentClientResult::http_error;
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

        if (cancelled)
            return AgentClientResult::cancelled;

        if (accept_event_stream)
            return AgentClientResult::success;

        if (!on_response(response_body, false))
            return AgentClientResult::cancelled;

        return AgentClientResult::success;
    }
}

AgentClientResult streamChatTurn(
    const std::string& base_url,
    const AgentChatTurnRequest& request,
    const AgentStreamEventCallback& on_event,
    std::string& out_error)
{
    ParsedEndpoint endpoint {};
    if (!parse_base_url(base_url, endpoint, out_error))
        return AgentClientResult::parse_error;

    const std::string path = endpoint.path_prefix + "/chat/v1/turns";
    const std::string body = build_request_body(request, true);

    return send_http_request(
        base_url,
        "POST",
        path,
        body,
        true,
        [&](const std::string& data, bool is_stream) -> bool {
            if (!is_stream)
                return true;

            if (data == "[DONE]")
                return true;

            json payload;
            try
            {
                payload = json::parse(data);
            }
            catch (const json::exception& e)
            {
                out_error = std::string("agent SSE parse error: ") + e.what();
                return false;
            }

            if (!on_event)
                return true;
            return on_event(payload);
        },
        out_error);
}

AgentClientResult runChatTurnSync(
    const std::string& base_url,
    const AgentChatTurnRequest& request,
    std::string& out_content,
    std::string& out_model,
    std::string& out_error)
{
    ParsedEndpoint endpoint {};
    if (!parse_base_url(base_url, endpoint, out_error))
        return AgentClientResult::parse_error;

    const std::string path = endpoint.path_prefix + "/chat/v1/turns";
    const std::string body = build_request_body(request, false);
    std::string response_body;

    const auto result = send_http_request(
        base_url,
        "POST",
        path,
        body,
        false,
        [&](const std::string& data, bool /*is_stream*/) -> bool {
            response_body = data;
            return true;
        },
        out_error);

    if (result != AgentClientResult::success)
        return result;

    try
    {
        const json payload = json::parse(response_body);
        if (payload.contains("content") && payload["content"].is_string())
            out_content = payload["content"].get<std::string>();
        if (payload.contains("model") && payload["model"].is_string())
            out_model = payload["model"].get<std::string>();

        if (out_content.empty())
        {
            out_error = "agent response missing content";
            return AgentClientResult::parse_error;
        }
        return AgentClientResult::success;
    }
    catch (const json::exception& e)
    {
        out_error = std::string("agent response parse error: ") + e.what();
        return AgentClientResult::parse_error;
    }
}

namespace
{
    AgentClientResult poll_sleep_job(
        const std::string& base_url,
        const std::string& job_id,
        AgentSleepJobResult& out_result,
        std::string& out_error)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(10);
        double wait_s = 2.0;

        while (std::chrono::steady_clock::now() < deadline)
        {
            ParsedEndpoint endpoint;
            if (!parse_base_url(base_url, endpoint, out_error))
                return AgentClientResult::parse_error;

            const std::string path = endpoint.path_prefix + "/sleep/v1/jobs/" + job_id;
            std::string response_body;
            const auto result = send_http_request(
                base_url,
                "GET",
                path,
                "",
                false,
                [&](const std::string& data, bool) -> bool {
                    response_body = data;
                    return true;
                },
                out_error);

            if (result != AgentClientResult::success)
                return result;

            try
            {
                const json payload = json::parse(response_body);
                const std::string status = payload.value("status", "");
                if (status == "done")
                {
                    const json& job_result = payload.at("result");
                    if (job_result.contains("reportText") && job_result["reportText"].is_string())
                        out_result.text = job_result["reportText"].get<std::string>();
                    else if (job_result.contains("summaryText") && job_result["summaryText"].is_string())
                        out_result.text = job_result["summaryText"].get<std::string>();

                    if (job_result.contains("embedding") && job_result["embedding"].is_array())
                    {
                        out_result.embedding.clear();
                        for (const auto& value : job_result["embedding"])
                            out_result.embedding.push_back(value.get<float>());
                    }

                    if (job_result.contains("model") && job_result["model"].is_string())
                        out_result.model = job_result["model"].get<std::string>();
                    if (job_result.contains("embeddingModel") && job_result["embeddingModel"].is_string())
                        out_result.embeddingModel = job_result["embeddingModel"].get<std::string>();

                    if (out_result.text.empty())
                    {
                        out_error = "sleep job result missing text";
                        return AgentClientResult::parse_error;
                    }
                    return AgentClientResult::success;
                }

                if (status == "failed")
                {
                    if (payload.contains("error") && payload["error"].is_object())
                    {
                        const auto& err = payload["error"];
                        out_error = err.value("code", "GENERATION_FAILED") + ": "
                            + err.value("message", "job failed");
                    }
                    else
                    {
                        out_error = "sleep job failed";
                    }
                    return AgentClientResult::http_error;
                }
            }
            catch (const json::exception& e)
            {
                out_error = std::string("sleep job poll parse error: ") + e.what();
                return AgentClientResult::parse_error;
            }

            const auto sleep_ms = static_cast<int64_t>(wait_s * 1000.0);
            std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
            if (wait_s < 7.0 && std::chrono::steady_clock::now() > deadline - std::chrono::minutes(9))
                wait_s = 7.0;
        }

        out_error = "sleep job poll timeout";
        return AgentClientResult::network_error;
    }
}

AgentClientResult runSleepJobSync(
    const std::string& base_url,
    const std::string& post_path,
    const json& body,
    AgentSleepJobResult& out_result,
    std::string& out_error)
{
    out_result = {};
    ParsedEndpoint endpoint;
    if (!parse_base_url(base_url, endpoint, out_error))
        return AgentClientResult::parse_error;

    const std::string path = endpoint.path_prefix + post_path;
    std::string response_body;
    const auto post_result = send_http_request(
        base_url,
        "POST",
        path,
        body.dump(),
        false,
        [&](const std::string& data, bool) -> bool {
            response_body = data;
            return true;
        },
        out_error);

    if (post_result != AgentClientResult::success)
        return post_result;

    try
    {
        const json payload = json::parse(response_body);
        if (!payload.contains("jobId") || !payload["jobId"].is_string())
        {
            out_error = "sleep job response missing jobId";
            return AgentClientResult::parse_error;
        }
        return poll_sleep_job(base_url, payload["jobId"].get<std::string>(), out_result, out_error);
    }
    catch (const json::exception& e)
    {
        out_error = std::string("sleep job response parse error: ") + e.what();
        return AgentClientResult::parse_error;
    }
}

SERVICE_NAMESPACE_END
WAVE_NAMESPACE_END
