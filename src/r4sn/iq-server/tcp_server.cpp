#include "tcp_server.hpp"

#include <cstring>
#include <iostream>
#include <span>
#include <vector>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <r4sn/iq_protocol.h>

namespace r4sn
{
    namespace
    {
        constexpr int kSyncTimeoutMs = 5000;
        constexpr int kListenBacklog = 8;
        constexpr std::size_t kMaxRequestBytes = 8192;
    }  // namespace

    TcpServer::TcpServer(const uint16_t port, const SyncMode sync_mode, const uint16_t sync_port) :
        m_port(port),
        m_sync_mode(sync_mode)
    {
        if (m_sync_mode == SyncMode::R4sn)
            m_frame_sync = std::make_unique<FrameSync>(sync_port);
    }

    TcpServer::~TcpServer()
    {
        if (m_listen_fd >= 0)
            close(m_listen_fd);
    }

    void TcpServer::run()
    {
        m_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (m_listen_fd < 0)
            throw std::runtime_error("failed to create listen socket");

        int reuse = 1;
        setsockopt(m_listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(m_port);
        addr.sin_addr.s_addr = htonl(INADDR_ANY);

        if (bind(m_listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
            throw std::runtime_error("failed to bind iq-server port");
        if (listen(m_listen_fd, kListenBacklog) < 0)
            throw std::runtime_error("failed to listen on iq-server port");

        std::cout << "iq-server listening on port " << m_port << '\n';

        while (true)
        {
            const int client_fd = accept(m_listen_fd, nullptr, nullptr);

            if (client_fd < 0) continue;
            while (handleClient(client_fd));

            close(client_fd);
        }
    }

    bool TcpServer::recvExact(const int client_fd, uint8_t* data, const size_t size)
    {
        for (size_t received = 0; received < size;)
        {
            const ssize_t n = recv(client_fd, data + received, size - received, 0);

            if (n <= 0) return false;
            received += static_cast<size_t>(n);
        }

        return true;
    }

    bool TcpServer::sendAll(const int client_fd, const uint8_t* data, const size_t size)
    {
        for (size_t sent = 0; sent < size;)
        {
            const ssize_t n = send(client_fd, data + sent, size - sent, 0);

            if (n <= 0) return false;
            sent += static_cast<size_t>(n);
        }

        return true;
    }

    bool TcpServer::waitForFrameSync(const int client_fd, const uint16_t target_count)
    {
        if (m_sync_mode != SyncMode::R4sn || !m_frame_sync)
            return true;

        const uint64_t seq = m_frame_sync->frameSeq();
        if (m_frame_sync->waitForNextFrame(seq, kSyncTimeoutMs))
            return true;

        iq::IqResponse response{};
        response.header.magic = iq::kResponseMagic;
        response.header.version = iq::kProtocolVersion;
        response.header.target_count = target_count;
        response.header.status = static_cast<uint32_t>(iq::IqStatus::SyncTimeout);
        const auto bytes = iq::writeResponse(response);
        sendAll(client_fd, bytes.data(), bytes.size());
        return false;
    }

    bool TcpServer::handleClient(const int client_fd)
    {
        std::vector<uint8_t> buffer(kMaxRequestBytes);
        if (!recvExact(client_fd, buffer.data(), iq::kRequestHeaderV2Size))
            return false;

        const uint32_t magic = *reinterpret_cast<uint32_t*>(buffer.data());
        const uint16_t version = *reinterpret_cast<uint16_t*>(buffer.data() + 4);

        if (magic != iq::kRequestMagic)
        {
            iq::IqResponse response{};
            response.header.magic = iq::kResponseMagic;
            response.header.version = iq::kProtocolVersion;
            response.header.target_count = 0;
            response.header.status = static_cast<uint32_t>(iq::IqStatus::InvalidRequest);
            const auto bytes = iq::writeResponse(response);
            sendAll(client_fd, bytes.data(), bytes.size());
            return true;
        }

        if (version == iq::kProtocolVersion)
        {
            const uint16_t target_count = *reinterpret_cast<uint16_t*>(buffer.data() + 6);
            if (target_count == 0 || target_count > iq::kMaxTargets)
            {
                iq::IqResponse response{};
                response.header.magic = iq::kResponseMagic;
                response.header.version = iq::kProtocolVersion;
                response.header.target_count = 0;
                response.header.status = static_cast<uint32_t>(iq::IqStatus::InvalidRequest);
                const auto bytes = iq::writeResponse(response);
                sendAll(client_fd, bytes.data(), bytes.size());
                return true;
            }

            const std::size_t request_size = iq::requestBodySize(target_count);
            if (request_size > buffer.size()
                || !recvExact(client_fd, buffer.data() + iq::kRequestHeaderV2Size, request_size - iq::kRequestHeaderV2Size))
                return false;

            iq::IqRequest request{};
            std::string error;
            if (!iq::readRequest(std::span<const uint8_t>(buffer.data(), request_size), request, error))
            {
                iq::IqResponse response{};
                response.header.magic = iq::kResponseMagic;
                response.header.version = iq::kProtocolVersion;
                response.header.target_count = 0;
                response.header.status = static_cast<uint32_t>(iq::IqStatus::InvalidRequest);
                const auto bytes = iq::writeResponse(response);
                sendAll(client_fd, bytes.data(), bytes.size());
                return true;
            }

            if (!waitForFrameSync(client_fd, request.header.target_count))
                return true;

            iq::IqResponse response{};
            response.header.magic = iq::kResponseMagic;
            response.header.version = iq::kProtocolVersion;
            response.header.target_count = request.header.target_count;
            response.header.status = static_cast<uint32_t>(iq::IqStatus::Ok);
            response.payload = m_processor.processRequest(m_cube.rangeCube(), request.targets);

            const auto bytes = iq::writeResponse(response);
            return sendAll(client_fd, bytes.data(), bytes.size());
        }

        if (version == iq::kProtocolVersion2)
        {
            const uint8_t request_type = buffer[6];
            const uint8_t va_wire = buffer[7];

            if (request_type == static_cast<uint8_t>(iq::RequestType::TargetIq))
            {
                iq::TargetIqBodyV2 body{};
                if (!recvExact(client_fd, reinterpret_cast<uint8_t*>(&body), sizeof(body)))
                    return false;

                if (body.target_count == 0 || body.target_count > iq::kMaxTargets)
                {
                    iq::IqResponse response{};
                    response.header.magic = iq::kResponseMagic;
                    response.header.version = iq::kProtocolVersion2;
                    response.header.target_count = 0;
                    response.header.status = static_cast<uint32_t>(iq::IqStatus::InvalidRequest);
                    const auto bytes = iq::writeResponse(response);
                    sendAll(client_fd, bytes.data(), bytes.size());
                    return true;
                }

                const std::string va_error = iq::validateTargetIqOptions(va_wire, body.tile, body.sub_ant);
                if (!va_error.empty())
                {
                    iq::IqResponse response{};
                    response.header.magic = iq::kResponseMagic;
                    response.header.version = iq::kProtocolVersion2;
                    response.header.target_count = 0;
                    response.header.status = static_cast<uint32_t>(iq::IqStatus::InvalidRequest);
                    const auto bytes = iq::writeResponse(response);
                    sendAll(client_fd, bytes.data(), bytes.size());
                    return true;
                }

                iq::IqRequest request{};
                request.header.magic = magic;
                request.header.version = version;
                request.header.target_count = body.target_count;
                request.targets.resize(body.target_count);

                if (!recvExact(
                        client_fd,
                        reinterpret_cast<uint8_t*>(request.targets.data()),
                        request.targets.size() * sizeof(iq::TargetSpec)))
                    return false;

                const std::string error = iq::validateRequest(request.header, request.targets);
                if (!error.empty())
                {
                    iq::IqResponse response{};
                    response.header.magic = iq::kResponseMagic;
                    response.header.version = iq::kProtocolVersion2;
                    response.header.target_count = 0;
                    response.header.status = static_cast<uint32_t>(iq::IqStatus::InvalidRequest);
                    const auto bytes = iq::writeResponse(response);
                    sendAll(client_fd, bytes.data(), bytes.size());
                    return true;
                }

                if (!waitForFrameSync(client_fd, request.header.target_count))
                    return true;

                const iq::VaCombineMode va_mode = iq::targetIqVaModeFromWire(va_wire);
                iq::IqResponse response{};
                response.header.magic = iq::kResponseMagic;
                response.header.version = iq::kProtocolVersion2;
                response.header.target_count = request.header.target_count;
                response.header.status = static_cast<uint32_t>(iq::IqStatus::Ok);
                response.payload = m_processor.processRequest(
                    m_cube.rangeCube(),
                    request.targets,
                    va_mode,
                    body.tile,
                    body.sub_ant);

                const auto bytes = iq::writeResponse(response);
                return sendAll(client_fd, bytes.data(), bytes.size());
            }

            if (request_type == static_cast<uint8_t>(iq::RequestType::RangeDopplerMap))
            {
                iq::RdmRequestSpec rdm_spec{};
                if (!recvExact(client_fd, reinterpret_cast<uint8_t*>(&rdm_spec), sizeof(rdm_spec)))
                    return false;

                const std::string error = iq::validateRdmRequest(rdm_spec);
                if (!error.empty())
                {
                    iq::RdmResponse response{};
                    response.header.magic = iq::kResponseMagic;
                    response.header.version = iq::kProtocolVersion2;
                    response.header.request_type = static_cast<uint16_t>(iq::RequestType::RangeDopplerMap);
                    response.header.status = static_cast<uint32_t>(iq::IqStatus::InvalidRequest);
                    const auto bytes = iq::writeRdmResponse(response);
                    sendAll(client_fd, bytes.data(), bytes.size());
                    return true;
                }

                if (!waitForFrameSync(client_fd, 0))
                    return true;

                const iq::RdmResponse rdm_response = m_processor.processRdm(m_cube.dopplerCube(), rdm_spec);
                const auto bytes = iq::writeRdmResponse(rdm_response);
                return sendAll(client_fd, bytes.data(), bytes.size());
            }

            iq::IqResponse response{};
            response.header.magic = iq::kResponseMagic;
            response.header.version = iq::kProtocolVersion2;
            response.header.target_count = 0;
            response.header.status = static_cast<uint32_t>(iq::IqStatus::InvalidRequest);
            const auto bytes = iq::writeResponse(response);
            sendAll(client_fd, bytes.data(), bytes.size());
            return true;
        }

        iq::IqResponse response{};
        response.header.magic = iq::kResponseMagic;
        response.header.version = version;
        response.header.target_count = 0;
        response.header.status = static_cast<uint32_t>(iq::IqStatus::InvalidRequest);
        const auto bytes = iq::writeResponse(response);
        sendAll(client_fd, bytes.data(), bytes.size());
        return true;
    }
}  // namespace r4sn
