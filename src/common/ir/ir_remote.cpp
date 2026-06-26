#include "ir/ir_remote.h"

#include <fcntl.h>
#include <unistd.h>

#include <cctype>
#include <cstring>
#include <mutex>

#define PROPAGATE_ERROR(res) \
    do { \
        if ((res) != IRResult::SUCCESS) { \
            return (res); \
        } \
    } while (0)

namespace
{
constexpr uint32_t NEC_HEADER_MARK_US = 9000;
constexpr uint32_t NEC_HEADER_SPACE_US = 4500;
constexpr uint32_t NEC_REPEAT_SPACE_US = 2250;
constexpr uint32_t NEC_SHORT_US = 560;
constexpr uint32_t NEC_LONG_US = 1690;
constexpr uint32_t NEC_MARGIN = 200;
constexpr size_t NEC_FRAME_PULSES = 67;
constexpr size_t NEC_REPEAT_PULSES = 4;

bool inRange(uint32_t val, uint32_t target)
{
    return target - NEC_MARGIN <= val && val <= target + NEC_MARGIN;
}

bool isHexDigit(char c)
{
    return std::isxdigit(static_cast<unsigned char>(c)) != 0;
}

bool parseHexNibble(char c, uint8_t& out)
{
    if (c >= '0' && c <= '9') {
        out = static_cast<uint8_t>(c - '0');
        return true;
    }
    if (c >= 'a' && c <= 'f') {
        out = static_cast<uint8_t>(c - 'a' + 10);
        return true;
    }
    if (c >= 'A' && c <= 'F') {
        out = static_cast<uint8_t>(c - 'A' + 10);
        return true;
    }
    return false;
}

bool parseByte(std::string_view str, uint8_t& out)
{
    out = 0;

    if (str.size() >= 2 && (str[0] == '0') && (str[1] == 'x' || str[1] == 'X')) {
        if (str.size() != 4)
            return false;
        uint8_t hi = 0;
        uint8_t lo = 0;
        if (!parseHexNibble(str[2], hi) || !parseHexNibble(str[3], lo))
            return false;
        out = static_cast<uint8_t>((hi << 4) | lo);
        return true;
    }

    if (str.size() == 2 && isHexDigit(str[0]) && isHexDigit(str[1])) {
        uint8_t hi = 0;
        uint8_t lo = 0;
        if (!parseHexNibble(str[0], hi) || !parseHexNibble(str[1], lo))
            return false;
        out = static_cast<uint8_t>((hi << 4) | lo);
        return true;
    }

    if (str.size() >= 2 && str[0] == '0' && (str[1] == 'b' || str[1] == 'B')) {
        if (str.size() != 10)
            return false;
        uint8_t res = 0;
        for (size_t i = 2; i < 10; ++i) {
            if (str[i] != '0' && str[i] != '1')
                return false;
            res = static_cast<uint8_t>((res << 1) | (str[i] - '0'));
        }
        out = res;
        return true;
    }

    if (str.size() == 8) {
        uint8_t res = 0;
        for (char c : str) {
            if (c != '0' && c != '1')
                return false;
            res = static_cast<uint8_t>((res << 1) | (c - '0'));
        }
        out = res;
        return true;
    }

    return false;
}

IRResult inBitRange(uint32_t val)
{
    if (inRange(val, NEC_SHORT_US) || inRange(val, NEC_LONG_US))
        return IRResult::SUCCESS;
    return IRResult::ERROR_FAILED_TO_RECEIVE;
}

constexpr uint32_t kPayloadTypeShift = 30;

uint32_t makePayloadValue(IRPayload::Type type, uint8_t code, uint8_t data)
{
    return (static_cast<uint32_t>(type) << kPayloadTypeShift)
           | (static_cast<uint32_t>(data) << 8)
           | static_cast<uint32_t>(code);
}
}  // namespace

// ============================================================================
// IRPayload
// ============================================================================

IRPayload IRPayload::repeatCode()
{
    IRPayload payload;
    payload.m_value = static_cast<uint32_t>(Type::REPEAT) << kTypeShift;
    return payload;
}

IRPayload::IRPayload() = default;

IRPayload::IRPayload(uint8_t code)
    : m_value(makePayloadValue(Type::CODE_ONLY, code, 0)) {}

IRPayload::IRPayload(uint8_t code, uint8_t data)
    : m_value(makePayloadValue(Type::CODE_DATA, code, data)) {}

IRPayload::IRPayload(std::string_view code)
{
    uint8_t parsed = 0;
    if (!parseByte(code, parsed))
        return;
    m_value = makePayloadValue(Type::CODE_ONLY, parsed, 0);
}

IRPayload::IRPayload(std::string_view code, std::string_view data)
{
    uint8_t parsedCode = 0;
    uint8_t parsedData = 0;
    if (!parseByte(code, parsedCode) || !parseByte(data, parsedData))
        return;
    m_value = makePayloadValue(Type::CODE_DATA, parsedCode, parsedData);
}

IRPayload::Type IRPayload::type() const
{
    return static_cast<Type>((m_value >> kTypeShift) & 0x3u);
}

uint8_t IRPayload::code() const
{
    return static_cast<uint8_t>(m_value & 0xFFu);
}

uint8_t IRPayload::data() const
{
    return static_cast<uint8_t>((m_value >> 8) & 0xFFu);
}

uint32_t IRPayload::raw() const
{
    const uint8_t c = code();
    const uint8_t d = data();
    return static_cast<uint32_t>(c)
           | (static_cast<uint32_t>(static_cast<uint8_t>(~c)) << 8)
           | (static_cast<uint32_t>(d) << 16)
           | (static_cast<uint32_t>(static_cast<uint8_t>(~d)) << 24);
}

bool IRPayload::valid() const
{
    return type() != Type::EMPTY;
}

bool IRPayload::operator==(const IRPayload& other) const
{
    return m_value == other.m_value;
}

bool IRPayload::operator!=(const IRPayload& other) const
{
    return !(*this == other);
}

// ============================================================================
// IRCommandList
// ============================================================================

IRCommandList::IRCommandList() = default;

IRCommandList::~IRCommandList() = default;

IRResult IRCommandList::addCommand(std::string_view name, const IRPayload& payload)
{
    if (!payload.valid() || payload.type() == IRPayload::Type::EMPTY)
        return IRResult::ERROR_CMD_EMPTY;

    if (m_commands.contains(std::string(name)))
        return IRResult::ERROR_CMD_ALREADY_EXISTS;

    m_commands[std::string(name)] = payload;
    return IRResult::SUCCESS;
}

IRResult IRCommandList::removeCommand(std::string_view name)
{
    return m_commands.erase(std::string(name)) > 0 ? IRResult::SUCCESS
                                                   : IRResult::ERROR_CMD_NOT_FOUND;
}

IRPayload& IRCommandList::operator[](std::string_view name)
{
    return m_commands[std::string(name)];
}

void IRCommandList::clear()
{
    m_commands.clear();
}

// ============================================================================
// IRTransmitter
// ============================================================================

IRTransmitter::IRTransmitter() = default;

IRTransmitter::IRTransmitter(std::shared_ptr<IRCommandList> cmd_list)
    : m_commandList(std::move(cmd_list)) {}

IRTransmitter::~IRTransmitter()
{
    shutdown();
}

void IRTransmitter::setCommandList(std::shared_ptr<IRCommandList> cmd_list)
{
    m_commandList = std::move(cmd_list);
}

const std::shared_ptr<IRCommandList>& IRTransmitter::getCommandList() const
{
    return m_commandList;
}

IRResult IRTransmitter::init(std::string_view dev)
{
    shutdown();

    const int fd = open(std::string(dev).c_str(), O_RDWR);
    if (fd < 0)
        return IRResult::ERROR_FAILED_TO_OPEN_DEVICE;

    m_fd = fd;
    return IRResult::SUCCESS;
}

void IRTransmitter::shutdown()
{
    if (m_fd >= 0) {
        close(m_fd);
        m_fd = -1;
    }
}

IRResult IRTransmitter::send(const IRPayload& payload) const
{
    if (m_fd < 0)
        return IRResult::ERROR_NOT_INITIALIZED;

    if (!payload.valid() || payload.type() == IRPayload::Type::EMPTY)
        return IRResult::ERROR_INVALID_FORMAT;

    if (payload.type() == IRPayload::Type::REPEAT) {
        const uint32_t rawBuf[NEC_REPEAT_PULSES] = {
            NEC_HEADER_MARK_US,
            NEC_REPEAT_SPACE_US,
            NEC_SHORT_US,
            NEC_SHORT_US,
        };
        return transmitRaw(const_cast<uint32_t*>(rawBuf), NEC_REPEAT_PULSES);
    }

    uint32_t rawBuf[NEC_FRAME_PULSES];
    size_t idx = 0;

    rawBuf[idx++] = NEC_HEADER_MARK_US;
    rawBuf[idx++] = NEC_HEADER_SPACE_US;

    const uint8_t payloadBytes[4] = {
        payload.code(),
        static_cast<uint8_t>(~payload.code()),
        payload.data(),
        static_cast<uint8_t>(~payload.data()),
    };

    for (int byteIdx = 0; byteIdx < 4; ++byteIdx) {
        const uint8_t currentByte = payloadBytes[byteIdx];
        for (int bitIdx = 0; bitIdx < 8; ++bitIdx) {
            if ((currentByte >> bitIdx) & 1) {
                rawBuf[idx++] = NEC_SHORT_US;
                rawBuf[idx++] = NEC_LONG_US;
            } else {
                rawBuf[idx++] = NEC_SHORT_US;
                rawBuf[idx++] = NEC_SHORT_US;
            }
        }
    }

    rawBuf[idx++] = NEC_SHORT_US;
    return transmitRaw(rawBuf, idx);
}

IRResult IRTransmitter::send(std::string_view cmd_name) const
{
    if (!m_commandList)
        return IRResult::ERROR_CMD_NOT_FOUND;

    const IRPayload* payload = nullptr;
    for (const auto& [name, registered] : *m_commandList) {
        if (name == cmd_name) {
            payload = &registered;
            break;
        }
    }

    if (!payload)
        return IRResult::ERROR_CMD_NOT_FOUND;
    if (payload->type() != IRPayload::Type::CODE_ONLY
        && payload->type() != IRPayload::Type::CODE_DATA) {
        return IRResult::ERROR_CMD_DONT_MATCH;
    }

    return send(*payload);
}

IRResult IRTransmitter::send(std::string_view cmd_name, uint8_t data) const
{
    if (!m_commandList)
        return IRResult::ERROR_CMD_NOT_FOUND;

    IRPayload registered;
    bool found = false;
    for (const auto& [name, payload] : *m_commandList) {
        if (name == cmd_name) {
            registered = payload;
            found = true;
            break;
        }
    }

    if (!found)
        return IRResult::ERROR_CMD_NOT_FOUND;

    if (registered.type() != IRPayload::Type::CODE_ONLY)
        return IRResult::ERROR_CMD_DONT_MATCH;

    return send(IRPayload(registered.code(), data));
}

IRResult IRTransmitter::send(std::string_view cmd_name, std::string_view data_str) const
{
    uint8_t data = 0;
    if (!parseByte(data_str, data))
        return IRResult::ERROR_INVALID_FORMAT;

    return send(cmd_name, data);
}

IRResult IRTransmitter::transmitRaw(uint32_t* buf, size_t count) const
{
    if (m_fd < 0)
        return IRResult::ERROR_NOT_INITIALIZED;

    if (write(m_fd, buf, count * sizeof(uint32_t)) < 0)
        return IRResult::ERROR_FAILED_TO_TRANSMIT;

    return IRResult::SUCCESS;
}

// ============================================================================
// IRReceiver
// ============================================================================

IRReceiver::IRReceiver() = default;

IRReceiver::IRReceiver(std::shared_ptr<IRCommandList> cmd_list)
    : m_commandList(std::move(cmd_list)) {}

IRReceiver::~IRReceiver()
{
    shutdown();
}

void IRReceiver::setCommandList(std::shared_ptr<IRCommandList> cmd_list)
{
    m_commandList = std::move(cmd_list);
}

const std::shared_ptr<IRCommandList>& IRReceiver::getCommandList() const
{
    return m_commandList;
}

IRResult IRReceiver::init(std::string_view dev)
{
    shutdown();

    const int fd = open(std::string(dev).c_str(), O_RDONLY);
    if (fd < 0)
        return IRResult::ERROR_FAILED_TO_OPEN_DEVICE;

    m_fd = fd;
    m_running = true;
    m_thread = std::thread(&IRReceiver::recvThreadFunc, this);
    return IRResult::SUCCESS;
}

void IRReceiver::shutdown()
{
    m_running = false;
    if (m_thread.joinable())
        m_thread.join();

    if (m_fd >= 0) {
        close(m_fd);
        m_fd = -1;
    }

    std::unique_lock lock(m_mutex);
    m_buffer.clear();
}

bool IRReceiver::hasData() const
{
    std::shared_lock lock(m_mutex);
    IRPayload payload;
    size_t consumed = 0;
    return tryParseFrame(payload, consumed);
}

void IRReceiver::recvThreadFunc()
{
    uint32_t readBuf[128];

    while (m_running) {
        if (m_fd < 0)
            break;

        const ssize_t bytesRead = read(m_fd, readBuf, sizeof(readBuf));
        if (bytesRead <= 0)
            continue;

        const size_t count = static_cast<size_t>(bytesRead) / sizeof(uint32_t);
        if (count == 0)
            continue;

        std::unique_lock lock(m_mutex);
        m_buffer.insert(m_buffer.end(), readBuf, readBuf + count);
    }
}

bool IRReceiver::tryParseFrame(IRPayload& out_payload, size_t& consumed) const
{
    consumed = 0;

    for (size_t start = 0; start + NEC_REPEAT_PULSES <= m_buffer.size(); ++start) {
        if (inRange(m_buffer[start], NEC_HEADER_MARK_US)
            && inRange(m_buffer[start + 1], NEC_REPEAT_SPACE_US)
            && inRange(m_buffer[start + 2], NEC_SHORT_US)
            && inRange(m_buffer[start + 3], NEC_SHORT_US)) {
            out_payload = IRPayload::repeatCode();
            consumed = start + NEC_REPEAT_PULSES;
            return true;
        }
    }

    for (size_t start = 0; start + NEC_FRAME_PULSES <= m_buffer.size(); ++start) {
        if (!inRange(m_buffer[start], NEC_HEADER_MARK_US)
            || !inRange(m_buffer[start + 1], NEC_HEADER_SPACE_US)) {
            continue;
        }

        uint32_t payloadBits = 0;
        size_t idx = start + 2;
        bool valid = true;

        for (int bit = 0; bit < 32; ++bit, idx += 2) {
            const uint32_t mark = m_buffer[idx];
            const uint32_t space = m_buffer[idx + 1];

            if (inBitRange(mark) != IRResult::SUCCESS
                || inBitRange(space) != IRResult::SUCCESS) {
                valid = false;
                break;
            }

            if (space > (NEC_SHORT_US + NEC_LONG_US) / 2)
                payloadBits |= (1U << bit);
        }

        if (!valid)
            continue;

        if (!inRange(m_buffer[idx], NEC_SHORT_US))
            continue;

        const uint8_t code = static_cast<uint8_t>(payloadBits & 0xFFu);
        const uint8_t codeInv = static_cast<uint8_t>((payloadBits >> 8) & 0xFFu);
        const uint8_t data = static_cast<uint8_t>((payloadBits >> 16) & 0xFFu);
        const uint8_t dataInv = static_cast<uint8_t>((payloadBits >> 24) & 0xFFu);

        if ((code ^ codeInv) != 0xFF || (data ^ dataInv) != 0xFF)
            continue;

        if (data == 0)
            out_payload = IRPayload(code);
        else
            out_payload = IRPayload(code, data);

        consumed = start + NEC_FRAME_PULSES;
        return true;
    }

    return false;
}

IRResult IRReceiver::recv(IRPayload& out_payload)
{
    std::unique_lock lock(m_mutex);

    size_t consumed = 0;
    if (!tryParseFrame(out_payload, consumed))
        return IRResult::ERROR_INCOMPLETE_FRAME;

    m_buffer.erase(m_buffer.begin(), m_buffer.begin() + static_cast<std::ptrdiff_t>(consumed));
    return IRResult::SUCCESS;
}

IRResult IRReceiver::recv(IRPayload& out_payload, std::string& out_cmd_name)
{
    PROPAGATE_ERROR(recv(out_payload));

    if (!m_commandList)
        return IRResult::ERROR_CMD_NOT_FOUND;

    for (const auto& [name, registered] : *m_commandList) {
        if (registered.type() == IRPayload::Type::CODE_ONLY
            && registered.code() == out_payload.code()) {
            out_cmd_name = name;
            return IRResult::SUCCESS;
        }

        if (registered.type() == IRPayload::Type::CODE_DATA
            && registered.code() == out_payload.code()
            && registered.data() == out_payload.data()) {
            out_cmd_name = name;
            return IRResult::SUCCESS;
        }
    }

    return IRResult::ERROR_CMD_NOT_FOUND;
}
