#include "ir/ir_remote.h"

#include <fcntl.h>
#include <unistd.h>

#include <cctype>
#include <cstring>
#include <mutex>
#include <poll.h>
#include <fstream>
#include <sstream>
#include <vector>
#include <algorithm>
#include <iomanip>

#define PROPAGATE_ERROR(res) \
    do { \
        if ((res) != IRResult::SUCCESS) { \
            return (res); \
        } \
    } while (0)

namespace
{
    constexpr size_t NEC_FRAME_PULSES = 67;
    constexpr size_t NEC_REPEAT_PULSES = 4;

    constexpr size_t kMaxBufferEntries = 512;

    bool in_range(uint32_t val, uint32_t target, uint32_t margin = 200)
    {
        const uint32_t min_val = (target > margin) ? (target - margin) : 0;
        return min_val <= val && val <= target + margin;
    }

    bool is_hex_digit(char c)
    {
        return std::isxdigit(static_cast<unsigned char>(c)) != 0;
    }

    bool parse_hex_nibble(char c, uint8_t& out)
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

    bool parse_byte(std::string_view str, uint8_t& out)
    {
        out = 0;

        if (str.size() >= 2 && (str[0] == '0') && (str[1] == 'x' || str[1] == 'X')) {
            if (str.size() != 4)
                return false;
            uint8_t hi = 0;
            uint8_t lo = 0;
            if (!parse_hex_nibble(str[2], hi) || !parse_hex_nibble(str[3], lo))
                return false;
            out = static_cast<uint8_t>((hi << 4) | lo);
            return true;
        }

        if (str.size() == 2 && is_hex_digit(str[0]) && is_hex_digit(str[1])) {
            uint8_t hi = 0;
            uint8_t lo = 0;
            if (!parse_hex_nibble(str[0], hi) || !parse_hex_nibble(str[1], lo))
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

    IRResult in_bit_range(uint32_t val)
    {
        if (in_range(val, NEC_TIMINGS.bitMark) || in_range(val, NEC_TIMINGS.bitOneSpace))
            return IRResult::SUCCESS;
        return IRResult::ERROR_FAILED_TO_RECEIVE;
    }

}  // namespace

const IRProtocolTimings NEC_TIMINGS = {
    9000, // headerMark
    4500, // headerSpace
    2250, // repeatSpace
    560,  // bitMark
    1690, // bitOneSpace
    560,  // bitZeroSpace
    200,  // margin
    200   // headerMargin
};

const IRProtocolTimings LG_AC_TIMINGS = {
    9000, // headerMark
    4500, // headerSpace
    2250, // repeatSpace
    560,  // bitMark
    1690, // bitOneSpace
    560,  // bitZeroSpace
    200,  // margin
    600   // headerMargin (relaxed)
};

const char* resultToString(IRResult result)
{
    switch (result) {
    case IRResult::SUCCESS:
        return "SUCCESS";
    case IRResult::ERROR_INVALID_FORMAT:
        return "ERROR_INVALID_FORMAT";
    case IRResult::ERROR_INVALID_LENGTH:
        return "ERROR_INVALID_LENGTH";
    case IRResult::ERROR_FAILED_TO_OPEN_DEVICE:
        return "ERROR_FAILED_TO_OPEN_DEVICE";
    case IRResult::ERROR_NOT_INITIALIZED:
        return "ERROR_NOT_INITIALIZED";
    case IRResult::ERROR_CMD_ALREADY_EXISTS:
        return "ERROR_CMD_ALREADY_EXISTS";
    case IRResult::ERROR_CMD_NOT_FOUND:
        return "ERROR_CMD_NOT_FOUND";
    case IRResult::ERROR_CMD_EMPTY:
        return "ERROR_CMD_EMPTY";
    case IRResult::ERROR_CMD_DONT_MATCH:
        return "ERROR_CMD_DONT_MATCH";
    case IRResult::ERROR_INCOMPLETE_FRAME:
        return "ERROR_INCOMPLETE_FRAME";
    case IRResult::ERROR_FAILED_TO_RECEIVE:
        return "ERROR_FAILED_TO_RECEIVE";
    case IRResult::ERROR_FAILED_TO_TRANSMIT:
        return "ERROR_FAILED_TO_TRANSMIT";
    }
    return "UNKNOWN";
}

// ============================================================================
// IRPayload
// ============================================================================

IRPayload IRPayload::repeatCode()
{
    IRPayload payload;
    payload.m_type = Type::REPEAT;
    payload.m_value = 0;
    return payload;
}

IRPayload IRPayload::fromRaw28(uint32_t raw28)
{
    IRPayload payload;
    payload.m_type = Type::LG_AC_DATA;
    payload.m_value = raw28;
    return payload;
}

IRPayload::IRPayload() = default;

IRPayload::IRPayload(uint8_t code)
    : m_type(Type::CODE_ONLY), m_value(code) {}

IRPayload::IRPayload(uint8_t code, uint8_t data)
    : m_type(Type::CODE_DATA), m_value((static_cast<uint32_t>(data) << 8) | code) {}

IRPayload::IRPayload(std::string_view code)
{
    uint8_t parsed = 0;
    if (!parse_byte(code, parsed))
        return;
    m_type = Type::CODE_ONLY;
    m_value = parsed;
}

IRPayload::IRPayload(std::string_view code, std::string_view data)
{
    uint8_t parsedCode = 0;
    uint8_t parsedData = 0;
    if (!parse_byte(code, parsedCode) || !parse_byte(data, parsedData))
        return;
    m_type = Type::CODE_DATA;
    m_value = (static_cast<uint32_t>(parsedData) << 8) | parsedCode;
}

IRPayload::Type IRPayload::type() const
{
    return m_type;
}

IRPayload::DataType IRPayload::dataType() const
{
    return static_cast<DataType>(static_cast<uint8_t>(m_type) & 0x03);
}

IRPayload::Protocol IRPayload::protocol() const
{
    return static_cast<Protocol>((static_cast<uint8_t>(m_type) >> 2) & 0x3F);
}

uint8_t IRPayload::code() const
{
    return static_cast<uint8_t>(m_value & 0xFFu);
}

uint8_t IRPayload::data() const
{
    return static_cast<uint8_t>((m_value >> 8) & 0xFFu);
}

uint32_t IRPayload::raw28() const
{
    return m_value;
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
    return m_type != Type::EMPTY;
}

bool IRPayload::operator==(const IRPayload& other) const
{
    return m_type == other.m_type && m_value == other.m_value;
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

IRResult IRCommandList::addCommand(std::string_view name, const IRPayload& payload, bool overwrite)
{
    if (!payload.valid() || payload.type() == IRPayload::Type::EMPTY)
        return IRResult::ERROR_CMD_EMPTY;

    if (!overwrite && m_commands.contains(std::string(name)))
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

const IRPayload* IRCommandList::getPayload(std::string_view name) const
{
    auto it = m_commands.find(std::string(name));
    if (it != m_commands.end()) {
        return &it->second;
    }
    return nullptr;
}

void IRCommandList::clear()
{
    m_commands.clear();
}

bool IRCommandList::loadFromFile(const std::string& filepath)
{
    std::ifstream in(filepath);
    if (!in.is_open()) {
        return false;
    }

    std::string line;
    while (std::getline(in, line)) {
        size_t first = line.find_first_not_of(" \t\r\n");
        if (first == std::string::npos || line[first] == '#') {
            continue;
        }

        std::stringstream ss(line);
        std::string name, code_str, data_str;

        if (!std::getline(ss, name, ',')) continue;
        if (!std::getline(ss, code_str, ',')) continue;
        std::getline(ss, data_str, ',');

        auto trim = [](std::string& s) {
            size_t start = s.find_first_not_of(" \t\r\n");
            if (start == std::string::npos) {
                s.clear();
                return;
            }
            size_t end = s.find_last_not_of(" \t\r\n");
            s = s.substr(start, end - start + 1);
        };
        trim(name);
        trim(code_str);
        trim(data_str);

        if (name.empty() || code_str.empty()) continue;

        IRPayload payload;
        if (code_str.starts_with("raw28:")) {
            uint32_t val = std::stoul(code_str.substr(6), nullptr, 16);
            payload = IRPayload::fromRaw28(val);
        } else if (!data_str.empty()) {
            payload = IRPayload(code_str, data_str);
        } else {
            payload = IRPayload(code_str);
        }

        if (payload.valid()) {
            removeCommand(name);
            addCommand(name, payload);
        }
    }
    return true;
}

bool IRCommandList::saveToFile(const std::string& filepath) const
{
    std::ofstream out(filepath);
    if (!out.is_open()) {
        return false;
    }

    auto sortedCommands = getSortedCommands();

    out << "# Format: name,code[,data]\n";
    for (const auto& [name, payload] : sortedCommands) {
        if (payload.type() == IRPayload::Type::CODE_ONLY) {
            out << name << ",0x" << std::hex << std::setw(2) << std::setfill('0')
                << static_cast<int>(payload.code()) << "\n";
        } else if (payload.type() == IRPayload::Type::CODE_DATA) {
            out << name << ",0x" << std::hex << std::setw(2) << std::setfill('0')
                << static_cast<int>(payload.code()) << ",0x" << std::setw(2) << std::setfill('0')
                << static_cast<int>(payload.data()) << "\n";
        } else if (payload.type() == IRPayload::Type::LG_AC_DATA) {
            out << name << ",raw28:0x" << std::hex << payload.raw28() << "\n";
        }
    }
    return true;
}

std::vector<std::pair<std::string, IRPayload>> IRCommandList::getSortedCommands() const
{
    std::vector<std::pair<std::string, IRPayload>> sorted(m_commands.begin(), m_commands.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });
    return sorted;
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

    if (payload.type() == IRPayload::Type::LG_AC_DATA) {
        uint32_t rawBuf[2 + 28 * 2 + 1];
        size_t idx = 0;
        rawBuf[idx++] = LG_AC_TIMINGS.headerMark;
        rawBuf[idx++] = LG_AC_TIMINGS.headerSpace;

        uint32_t val28 = payload.raw28();
        for (int bitIdx = 0; bitIdx < 28; ++bitIdx) {
            if ((val28 >> bitIdx) & 1) {
                rawBuf[idx++] = LG_AC_TIMINGS.bitMark;
                rawBuf[idx++] = LG_AC_TIMINGS.bitOneSpace;
            } else {
                rawBuf[idx++] = LG_AC_TIMINGS.bitMark;
                rawBuf[idx++] = LG_AC_TIMINGS.bitZeroSpace;
            }
        }
        rawBuf[idx++] = LG_AC_TIMINGS.bitMark;
        return transmitRaw(rawBuf, idx);
    }

    if (payload.type() == IRPayload::Type::REPEAT) {
        const uint32_t rawBuf[NEC_REPEAT_PULSES] = {
            NEC_TIMINGS.headerMark,
            NEC_TIMINGS.repeatSpace,
            NEC_TIMINGS.bitMark,
            NEC_TIMINGS.bitMark,
        };
        return transmitRaw(const_cast<uint32_t*>(rawBuf), NEC_REPEAT_PULSES);
    }

    uint32_t rawBuf[NEC_FRAME_PULSES];
    size_t idx = 0;

    rawBuf[idx++] = NEC_TIMINGS.headerMark;
    rawBuf[idx++] = NEC_TIMINGS.headerSpace;

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
                rawBuf[idx++] = NEC_TIMINGS.bitMark;
                rawBuf[idx++] = NEC_TIMINGS.bitOneSpace;
            } else {
                rawBuf[idx++] = NEC_TIMINGS.bitMark;
                rawBuf[idx++] = NEC_TIMINGS.bitZeroSpace;
            }
        }
    }

    rawBuf[idx++] = NEC_TIMINGS.bitMark;
    return transmitRaw(rawBuf, idx);
}

IRResult IRTransmitter::send(std::string_view cmd_name) const
{
    if (!m_commandList)
        return IRResult::ERROR_CMD_NOT_FOUND;

    const IRPayload* payload = m_commandList->getPayload(cmd_name);
    if (!payload)
        return IRResult::ERROR_CMD_NOT_FOUND;

    if (payload->type() == IRPayload::Type::EMPTY || payload->type() == IRPayload::Type::REPEAT) {
        return IRResult::ERROR_CMD_DONT_MATCH;
    }

    return send(*payload);
}

IRResult IRTransmitter::send(std::string_view cmd_name, uint8_t data) const
{
    if (!m_commandList)
        return IRResult::ERROR_CMD_NOT_FOUND;

    const IRPayload* registered = m_commandList->getPayload(cmd_name);
    if (!registered)
        return IRResult::ERROR_CMD_NOT_FOUND;

    if (registered->type() != IRPayload::Type::CODE_ONLY)
        return IRResult::ERROR_CMD_DONT_MATCH;

    return send(IRPayload(registered->code(), data));
}

IRResult IRTransmitter::send(std::string_view cmd_name, std::string_view data_str) const
{
    uint8_t data = 0;
    if (!parse_byte(data_str, data))
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
    m_pendingMark.reset();
}

bool IRReceiver::hasData() const
{
    std::shared_lock lock(m_mutex);
    IRPayload payload;
    size_t consumed = 0;
    return tryParseFrame(payload, consumed);
}

void IRReceiver::appendLircPackets(const uint32_t* raw, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        const uint32_t packet = raw[i];
        const uint32_t mode = packet & LIRC_MODE2_MASK;

        if (mode == LIRC_MODE2_TIMEOUT) {
            if (m_pendingMark.has_value()) {
                m_buffer.push_back(*m_pendingMark);
                m_buffer.push_back(packet & LIRC_VALUE_MASK);
                m_pendingMark.reset();
            }
            continue;
        }

        if (mode == LIRC_MODE2_FREQUENCY || mode == LIRC_MODE2_OVERFLOW)
            continue;

        const uint32_t duration = packet & LIRC_VALUE_MASK;
        if (duration == 0)
            continue;

        if (packet & LIRC_PULSE_BIT) {
            m_pendingMark = duration;
            continue;
        }

        if (m_pendingMark.has_value()) {
            m_buffer.push_back(*m_pendingMark);
            m_buffer.push_back(duration);
            m_pendingMark.reset();
        }
    }

    if (m_buffer.size() > kMaxBufferEntries)
        m_buffer.erase(m_buffer.begin(),
                       m_buffer.begin() + static_cast<std::ptrdiff_t>(m_buffer.size() / 2));
}

void IRReceiver::recvThreadFunc()
{
    uint32_t readBuf[128];
    struct pollfd pfd;
    pfd.fd = m_fd;
    pfd.events = POLLIN;

    while (m_running) {
        if (m_fd < 0)
            break;

        int pollRes = poll(&pfd, 1, 100); // Wait up to 100ms
        if (pollRes < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pollRes == 0) {
            // Timeout, check m_running and loop again
            continue;
        }

        const ssize_t bytesRead = read(m_fd, readBuf, sizeof(readBuf));
        if (bytesRead <= 0)
            continue;

        const size_t count = static_cast<size_t>(bytesRead) / sizeof(uint32_t);
        if (count == 0)
            continue;

        std::unique_lock lock(m_mutex);
        appendLircPackets(readBuf, count);
    }
}

bool IRReceiver::tryParseFrame(IRPayload& out_payload, size_t& consumed) const
{
    consumed = 0;

    for (size_t start = 0; start + 3 <= m_buffer.size(); ++start) {
        if (!in_range(m_buffer[start], LG_AC_TIMINGS.headerMark, LG_AC_TIMINGS.headerMargin)
            || !in_range(m_buffer[start + 1], NEC_TIMINGS.repeatSpace, 350)
            || !in_range(m_buffer[start + 2], NEC_TIMINGS.bitMark)) {
            continue;
        }

        out_payload = IRPayload::repeatCode();
        if (start + 4 <= m_buffer.size() && in_range(m_buffer[start + 3], NEC_TIMINGS.bitMark))
            consumed = start + 4;
        else
            consumed = start + 3;
        return true;
    }

    for (size_t start = 0; start + 2 <= m_buffer.size(); ++start) {
        if (!in_range(m_buffer[start], LG_AC_TIMINGS.headerMark, LG_AC_TIMINGS.headerMargin)
            || !in_range(m_buffer[start + 1], LG_AC_TIMINGS.headerSpace, LG_AC_TIMINGS.headerMargin)) {
            continue;
        }

        uint32_t payloadBits = 0;
        size_t idx = start + 2;
        int bitsParsed = 0;

        while (idx + 1 < m_buffer.size() && bitsParsed < 32) {
            const uint32_t mark = m_buffer[idx];
            const uint32_t space = m_buffer[idx + 1];

            if (in_bit_range(mark) != IRResult::SUCCESS
                || in_bit_range(space) != IRResult::SUCCESS) {
                break;
            }

            if (space > (NEC_TIMINGS.bitMark + NEC_TIMINGS.bitOneSpace) / 2)
                payloadBits |= (1U << bitsParsed);

            bitsParsed++;
            idx += 2;
        }

        if (bitsParsed != 28 && bitsParsed != 32) {
            continue;
        }

        if (idx >= m_buffer.size() || !in_range(m_buffer[idx], NEC_TIMINGS.bitMark)) {
            continue;
        }

        if (bitsParsed == 32) {
            const uint8_t code = static_cast<uint8_t>(payloadBits & 0xFFu);
            const uint8_t codeInv = static_cast<uint8_t>((payloadBits >> 8) & 0xFFu);
            const uint8_t data = static_cast<uint8_t>((payloadBits >> 16) & 0xFFu);
            const uint8_t dataInv = static_cast<uint8_t>((payloadBits >> 24) & 0xFFu);

            if ((code ^ codeInv) == 0xFF && (data ^ dataInv) == 0xFF) {
                if (data == 0)
                    out_payload = IRPayload(code);
                else
                    out_payload = IRPayload(code, data);
            } else {
                continue;
            }
        } else if (bitsParsed == 28) {
            out_payload = IRPayload::fromRaw28(payloadBits);
        }

        consumed = idx + 1;
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

        if (registered.type() == IRPayload::Type::LG_AC_DATA
            && registered.raw28() == out_payload.raw28()) {
            out_cmd_name = name;
            return IRResult::SUCCESS;
        }
    }

    return IRResult::ERROR_CMD_NOT_FOUND;
}
