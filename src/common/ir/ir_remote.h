#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

enum class IRResult
{
    SUCCESS = 0,
    ERROR_INVALID_FORMAT,
    ERROR_INVALID_LENGTH,
    ERROR_FAILED_TO_OPEN_DEVICE,
    ERROR_NOT_INITIALIZED,
    ERROR_CMD_ALREADY_EXISTS,
    ERROR_CMD_NOT_FOUND,
    ERROR_CMD_EMPTY,
    ERROR_CMD_DONT_MATCH,
    ERROR_INCOMPLETE_FRAME,
    ERROR_FAILED_TO_RECEIVE,
    ERROR_FAILED_TO_TRANSMIT,
};

class IRPayload
{
public:
    enum class Type : uint8_t
    {
        EMPTY = 0,
        CODE_ONLY = 1,
        CODE_DATA = 2,
        REPEAT = 3,
        CUSTOM_28 = 4,
    };

    static IRPayload repeatCode();
    static IRPayload fromRaw28(uint32_t raw28);

    IRPayload();
    IRPayload(uint8_t code);
    IRPayload(uint8_t code, uint8_t data);
    IRPayload(std::string_view code);
    IRPayload(std::string_view code, std::string_view data);

    Type type() const;

    uint8_t code() const;
    uint8_t data() const;
    uint32_t raw28() const;

    uint32_t raw() const;

    bool valid() const;

    bool operator==(const IRPayload& other) const;
    bool operator!=(const IRPayload& other) const;

private:
    static constexpr uint32_t kTypeShift = 29;

    uint32_t m_value = 0;
};

class IRCommandList
{
public:
    IRCommandList();
    IRCommandList(const IRCommandList&) = delete;
    IRCommandList& operator=(const IRCommandList&) = delete;
    ~IRCommandList();

    IRResult addCommand(std::string_view name, const IRPayload& payload);
    IRResult removeCommand(std::string_view name);

    IRPayload& operator[](std::string_view name);

    auto begin() const { return m_commands.begin(); }
    auto end() const { return m_commands.end(); }

    void clear();

private:
    std::unordered_map<std::string, IRPayload> m_commands;
};

class IRTransmitter
{
public:
    IRTransmitter();
    IRTransmitter(std::shared_ptr<IRCommandList> cmd_list);
    IRTransmitter(const IRTransmitter&) = delete;
    IRTransmitter& operator=(const IRTransmitter&) = delete;
    ~IRTransmitter();

    void setCommandList(std::shared_ptr<IRCommandList> cmd_list);
    const std::shared_ptr<IRCommandList>& getCommandList() const;

    IRResult init(std::string_view dev);
    void shutdown();

    IRResult send(const IRPayload& payload) const;

    IRResult send(std::string_view cmd_name) const;
    IRResult send(std::string_view cmd_name, uint8_t data) const;
    IRResult send(std::string_view cmd_name, std::string_view data_str) const;

private:
    IRResult transmitRaw(uint32_t* buf, size_t count) const;

    std::shared_ptr<IRCommandList> m_commandList;
    int m_fd = -1;
};

class IRReceiver
{
public:
    IRReceiver();
    IRReceiver(std::shared_ptr<IRCommandList> cmd_list);
    IRReceiver(const IRReceiver&) = delete;
    IRReceiver& operator=(const IRReceiver&) = delete;
    ~IRReceiver();

    void setCommandList(std::shared_ptr<IRCommandList> cmd_list);
    const std::shared_ptr<IRCommandList>& getCommandList() const;

    IRResult init(std::string_view dev);
    void shutdown();

    bool hasData() const;

    IRResult recv(IRPayload& out_payload);
    IRResult recv(IRPayload& out_payload, std::string& out_cmd_name);

private:
    bool tryParseFrame(IRPayload& out_payload, size_t& consumed) const;
    void appendLircPackets(const uint32_t* raw, size_t count);
    void recvThreadFunc();

    std::vector<uint32_t> m_buffer;
    std::optional<uint32_t> m_pendingMark;
    std::shared_ptr<IRCommandList> m_commandList;
    int m_fd = -1;

    std::thread m_thread;
    std::atomic<bool> m_running{false};
    mutable std::shared_mutex m_mutex;
};
