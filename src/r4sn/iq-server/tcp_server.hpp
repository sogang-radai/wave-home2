#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include "cube_mmap.hpp"
#include "frame_sync.hpp"
#include "signal_processor.hpp"

namespace r4sn
{
    enum class SyncMode
    {
        None,
        R4sn,
    };

    class TcpServer
    {
    public:
        TcpServer(uint16_t port, SyncMode sync_mode, uint16_t sync_port);
        ~TcpServer();

        TcpServer(const TcpServer&) = delete;
        TcpServer& operator=(const TcpServer&) = delete;

        void run();

    private:
        bool recvExact(int client_fd, uint8_t* data, size_t size);
        bool sendAll(int client_fd, const uint8_t* data, size_t size);
        bool waitForFrameSync(int client_fd, uint16_t target_count);
        bool handleClient(int client_fd);

        uint16_t m_port;
        SyncMode m_sync_mode;
        CubeMmap m_cube;
        SignalProcessor m_processor;
        std::unique_ptr<FrameSync> m_frame_sync;
        int m_listen_fd = -1;
    };
}  // namespace r4sn
