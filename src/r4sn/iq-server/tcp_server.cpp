#include "tcp_server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <iostream>
#include <span>
#include <vector>

#include <r4sn/iq_protocol.h>

namespace
{
    constexpr int kSyncTimeoutMs = 5000;
    constexpr int kListenBacklog = 8;
    constexpr size_t kMaxRequestBytes = 8192;
}

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

        if (client_fd < 0)
            continue;
        while (handleClient(client_fd));

        close(client_fd);
    }
}

bool TcpServer::recvExact(const int client_fd, uint8_t* data, const size_t size)
{
    for (size_t received = 0; received < size;)
    {
        const ssize_t n = recv(client_fd, data + received, size - received, 0);

        if (n <= 0)
            return false;
        received += static_cast<size_t>(n);
    }

    return true;
}

bool TcpServer::sendAll(const int client_fd, const uint8_t* data, const size_t size)
{
    for (size_t sent = 0; sent < size;)
    {
        const ssize_t n = send(client_fd, data + sent, size - sent, 0);

        if (n <= 0)
            return false;
        sent += static_cast<size_t>(n);
    }

    return true;
}

bool TcpServer::sendIqError(const int client_fd, const iq::IqStatus status, const uint16_t target_count)
{
    iq::IqResponse response{};
    response.header.status = static_cast<uint32_t>(status);
    response.info.target_count = target_count;
    const auto bytes = iq::writeIqResponse(response);
    return sendAll(client_fd, bytes.data(), bytes.size());
}

bool TcpServer::sendRdmError(const int client_fd, const iq::IqStatus status)
{
    iq::RdmResponse response{};
    response.header.status = static_cast<uint32_t>(status);
    const auto bytes = iq::writeRdmResponse(response);
    return sendAll(client_fd, bytes.data(), bytes.size());
}

bool TcpServer::waitForFrameSync(const int client_fd, const uint16_t target_count)
{
    if (m_sync_mode != SyncMode::R4sn || !m_frame_sync)
        return true;

    const uint64_t seq = m_frame_sync->frameSeq();
    if (m_frame_sync->waitForNextFrame(seq, kSyncTimeoutMs))
        return true;

    sendIqError(client_fd, iq::IqStatus::SyncTimeout, target_count);
    return false;
}

bool TcpServer::handleClient(const int client_fd)
{
    std::vector<uint8_t> buffer(kMaxRequestBytes);
    if (!recvExact(client_fd, buffer.data(), iq::kRequestHeaderSize))
        return false;

    iq::RequestHeader header{};
    std::memcpy(&header, buffer.data(), iq::kRequestHeaderSize);

    if (header.magic != iq::kRequestMagic)
    {
        sendIqError(client_fd, iq::IqStatus::InvalidRequest, 0);
        return true;
    }

    if (header.version != iq::kProtocolVersion)
    {
        sendIqError(client_fd, iq::IqStatus::InvalidRequest, 0);
        return true;
    }

    const size_t total_size = iq::kRequestHeaderSize + header.payload_size;
    if (total_size > buffer.size() || header.payload_size == 0)
    {
        sendIqError(client_fd, iq::IqStatus::InvalidRequest, 0);
        return true;
    }

    if (!recvExact(client_fd, buffer.data() + iq::kRequestHeaderSize, header.payload_size))
        return false;

    iq::RequestType type{};
    iq::IqRequestMsg iq_msg{};
    iq::RdmRequestMsg rdm_msg{};
    std::string error;
    if (!iq::readRequest(std::span<const uint8_t>(buffer.data(), total_size), type, iq_msg, rdm_msg, error))
    {
        if (type == iq::RequestType::Rdm)
            sendRdmError(client_fd, iq::IqStatus::InvalidRequest);
        else
            sendIqError(client_fd, iq::IqStatus::InvalidRequest, 0);
        return true;
    }

    if (type == iq::RequestType::Iq)
    {
        if (!waitForFrameSync(client_fd, iq_msg.request.target_count))
            return true;

        const iq::IqResponse response = m_processor.processIq(m_cube.rangeCube(), iq_msg);
        const auto bytes = iq::writeIqResponse(response);
        return sendAll(client_fd, bytes.data(), bytes.size());
    }

    if (type == iq::RequestType::Rdm)
    {
        if (!waitForFrameSync(client_fd, 0))
            return true;

        const iq::RdmResponse response = m_processor.processRdm(m_cube.dopplerCube(), rdm_msg);
        const auto bytes = iq::writeRdmResponse(response);
        return sendAll(client_fd, bytes.data(), bytes.size());
    }

    sendIqError(client_fd, iq::IqStatus::InvalidRequest, 0);
    return true;
}
