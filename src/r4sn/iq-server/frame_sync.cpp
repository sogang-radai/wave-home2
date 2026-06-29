#include "frame_sync.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <vector>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace
{
    constexpr std::size_t kNetHeaderSize = 36;
    constexpr std::size_t kMaxBuffer = 1024 * 1024;
    constexpr std::size_t kRecvChunk = 8192;
    constexpr uint32_t kMsgTypeData = 1;

    #pragma pack(push, 1)
    struct NetHeader {
        uint32_t msg_type;
        uint32_t magic;
        uint32_t reserved0;
        uint32_t reserved1;
        uint32_t reserved2;
        uint32_t vc_index;
        uint32_t payload_length;
        uint32_t reserved3;
        uint32_t padding;
    };
    #pragma pack(pop)

    static_assert(sizeof(NetHeader) == kNetHeaderSize);

     bool isFrameHeader(const NetHeader& header)
     {
         return header.msg_type == kMsgTypeData && header.magic == iq::kPcFrameMagic;
     }
}

FrameSync::FrameSync(const uint16_t port) : m_port(port)
{
    m_running.store(true, std::memory_order_release);
    m_thread = std::thread([this] { run(); });
}

FrameSync::~FrameSync()
{
    m_running.store(false, std::memory_order_release);

    if (m_sock >= 0)
    {
        shutdown(m_sock, SHUT_RDWR);
        close(m_sock);
        m_sock = -1;
    }

    if (m_thread.joinable())
        m_thread.join();
}

bool FrameSync::waitForNextFrame(const uint64_t after_seq, const int timeout_ms)
{
    using clock_t = std::chrono::steady_clock;

    const auto deadline = clock_t::now() + std::chrono::milliseconds(timeout_ms);

    while (m_frame_seq.load(std::memory_order_acquire) <= after_seq)
    {
        if (clock_t::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    return true;
}

bool FrameSync::handleBuffer()
{
    while (m_buffer.size() >= kNetHeaderSize)
    {
        NetHeader header{};
        std::memcpy(&header, m_buffer.data(), sizeof(header));

        if (!isFrameHeader(header))
        {
            m_buffer.erase(0, 1);
            continue;
        }

        const std::size_t frame_size = kNetHeaderSize + header.payload_length;
        if (m_buffer.size() < frame_size) return false;

        m_frame_seq.fetch_add(1, std::memory_order_acq_rel);
        m_buffer.erase(0, frame_size);
    }
    return true;
}

void FrameSync::run()
{
    while (m_running.load(std::memory_order_acquire))
    {
        m_sock = socket(AF_INET, SOCK_STREAM, 0);
        if (m_sock < 0)
        {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(m_port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        if (connect(m_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
        {
            close(m_sock);
            m_sock = -1;
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        const int flags = fcntl(m_sock, F_GETFL, 0);
        if (flags >= 0)
            fcntl(m_sock, F_SETFL, flags | O_NONBLOCK);

        m_buffer.clear();
        std::vector<char> chunk(kRecvChunk);

        while (m_running.load(std::memory_order_acquire))
        {
            const ssize_t n = recv(m_sock, chunk.data(), chunk.size(), 0);

            if (n > 0)
            {
                m_buffer.append(chunk.data(), static_cast<std::size_t>(n));
                if (m_buffer.size() > kMaxBuffer)
                    m_buffer.erase(0, m_buffer.size() - kMaxBuffer / 2);


                while (handleBuffer());
                continue;
            }

            if (n == 0) break;
            
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            break;
        }

        close(m_sock);
        m_sock = -1;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}
