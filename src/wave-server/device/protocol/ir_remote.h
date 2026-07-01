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
#include <deque>

#define IR_NAMESPACE_BEGIN namespace ir {
#define IR_NAMESPACE_END }

IR_NAMESPACE_BEGIN

enum class Result
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

const char* to_string(Result result);

class Payload
{
public:
    enum class Protocol : uint8_t
    {
        None = 0,
        Raw = 1,
        Nec = 2,
        LgAc = 3,
    };

    enum class Kind : uint8_t
    {
        Empty = 0,
        Repeat = 33,
        NecCodeOnly = 34,
        NecCodeData = 35,
        LgAc28 = 36,
    };

    static Payload repeatCode();
    static Payload fromRaw28(uint32_t raw28);
    static Payload fromRawBits(uint8_t bitCount, uint32_t bits);

    static bool isRawKind(Kind kind);
    static const char* kindToString(Kind kind);
    static const char* protocolToString(Protocol protocol);

    Payload();
    Payload(uint8_t code);
    Payload(uint8_t code, uint8_t data);
    Payload(std::string_view code);
    Payload(std::string_view code, std::string_view data);

    Kind kind() const;
    Protocol protocol() const;
    uint8_t bitCount() const;

    uint8_t code() const;
    uint8_t data() const;
    uint32_t rawBits() const;
    uint32_t raw28() const;
    uint32_t necWire32() const;

    bool isNec() const;
    bool isLgAc() const;
    bool isRaw() const;

    bool valid() const;
    bool matches(const Payload& other) const;

    bool operator==(const Payload& other) const;
    bool operator!=(const Payload& other) const;

private:
    static uint32_t maskBits(uint32_t value, uint8_t bitCount);

    Kind m_kind = Kind::Empty;
    uint32_t m_value = 0;
};

class CommandList
{
public:
    CommandList();
    CommandList(const CommandList&) = delete;
    CommandList& operator=(const CommandList&) = delete;
    ~CommandList();

    Result addCommand(std::string_view name, const Payload& payload, bool overwrite = true);
    Result removeCommand(std::string_view name);

    const Payload* getPayload(std::string_view name) const;

    auto begin() const { return m_commands.begin(); }
    auto end() const { return m_commands.end(); }

    void clear();

    bool loadFromFile(const std::string& filepath);
    bool saveToFile(const std::string& filepath) const;
    std::vector<std::pair<std::string, Payload>> getSortedCommands() const;

private:
    std::unordered_map<std::string, Payload> m_commands;
};

class Transmitter
{
public:
    Transmitter();
    Transmitter(std::shared_ptr<CommandList> cmd_list);
    Transmitter(const Transmitter&) = delete;
    Transmitter& operator=(const Transmitter&) = delete;
    ~Transmitter();

    void setCommandList(std::shared_ptr<CommandList> cmd_list);
    const std::shared_ptr<CommandList>& getCommandList() const;

    Result init(std::string_view dev);
    void shutdown();

    Result send(const Payload& payload) const;

    Result send(std::string_view cmd_name) const;
    Result send(std::string_view cmd_name, uint8_t data) const;
    Result send(std::string_view cmd_name, std::string_view data_str) const;

private:
    Result transmitRaw(uint32_t* buf, size_t count) const;

    std::shared_ptr<CommandList> m_commandList;
    int m_fd = -1;
};

class Receiver
{
public:
    Receiver();
    Receiver(std::shared_ptr<CommandList> cmd_list);
    Receiver(const Receiver&) = delete;
    Receiver& operator=(const Receiver&) = delete;
    ~Receiver();

    void setCommandList(std::shared_ptr<CommandList> cmd_list);
    const std::shared_ptr<CommandList>& getCommandList() const;

    Result init(std::string_view dev);
    void shutdown();

    bool hasData() const;

    Result recv(Payload& out_payload);
    Result recv(Payload& out_payload, std::string& out_cmd_name);

private:
    bool tryParseFrame(Payload& out_payload, size_t& consumed) const;
    void appendLircPackets(const uint32_t* raw, size_t count);
    void recvThreadFunc();

    std::deque<uint32_t> m_buffer;
    std::optional<uint32_t> m_pendingMark;
    std::shared_ptr<CommandList> m_commandList;
    int m_fd = -1;

    std::thread m_thread;
    std::atomic<bool> m_running{false};
    mutable std::shared_mutex m_mutex;
};

IR_NAMESPACE_END
