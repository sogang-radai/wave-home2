#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <r4sn/iq_protocol.h>

class FrameSync
{
public:
    explicit FrameSync(uint16_t port);
    ~FrameSync();

    FrameSync(const FrameSync&) = delete;
    FrameSync& operator=(const FrameSync&) = delete;

    bool waitForNextFrame(uint64_t after_seq, int timeout_ms);

    uint64_t frameSeq() const { return m_frame_seq.load(std::memory_order_acquire); }

private:
    void run();
    bool handleBuffer();

    uint16_t m_port;
    std::atomic<bool> m_running{false};
    std::atomic<uint64_t> m_frame_seq{0};
    std::thread m_thread;
    int m_sock = -1;
    std::string m_buffer;
};
