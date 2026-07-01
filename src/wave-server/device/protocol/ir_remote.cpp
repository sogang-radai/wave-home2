#include "device/protocol/ir_remote.h"

#include <fcntl.h>
#include <unistd.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <poll.h>
#include <sstream>
#include <vector>
#include <algorithm>

#include "core/json.h"

#define PROPAGATE_ERROR(res) \
    do { \
        if ((res) != ir::Result::SUCCESS) { \
            return (res); \
        } \
    } while (0)

IR_NAMESPACE_BEGIN

namespace
{
    constexpr size_t kNecFramePulses = 67;
    constexpr size_t kNecRepeatPulses = 4;
    constexpr size_t kMaxBufferEntries = 512;
    constexpr uint8_t kMaxRawBits = 32;

    constexpr uint32_t kLircPulseBit = 0x01000000u;
    constexpr uint32_t kLircMode2Mask = 0xFF000000u;
    constexpr uint32_t kLircValueMask = 0x00FFFFFFu;
    constexpr uint32_t kLircMode2Frequency = 0x02000000u;
    constexpr uint32_t kLircMode2Timeout = 0x03000000u;
    constexpr uint32_t kLircMode2Overflow = 0x04000000u;

    struct ProtocolTimings
    {
        uint32_t headerMark;
        uint32_t headerSpace;
        uint32_t repeatSpace;
        uint32_t bitMark;
        uint32_t bitOneSpace;
        uint32_t bitZeroSpace;
        uint32_t margin;
        uint32_t headerMargin;
    };

    const ProtocolTimings kNecTimings = {
        9000,
        4500,
        2250,
        560,
        1690,
        560,
        200,
        200,
    };

    const ProtocolTimings kLgAcTimings = {
        9000,
        4500,
        2250,
        560,
        1690,
        560,
        200,
        600,
    };

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

    bool parse_uint32(std::string_view str, uint32_t& out)
    {
        if (str.empty())
            return false;

        std::string text(str);
        size_t idx = 0;
        const int base = (text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) ? 16 : 10;
        try {
            out = static_cast<uint32_t>(std::stoul(text, &idx, base));
        } catch (...) {
            return false;
        }
        return idx == text.size();
    }

    uint8_t bit_count_for_kind(Payload::Kind kind)
    {
        const uint8_t value = static_cast<uint8_t>(kind);
        if (value >= 1 && value <= kMaxRawBits)
            return value;
        if (kind == Payload::Kind::LgAc28)
            return 28;
        return 0;
    }

    Result in_bit_range(uint32_t val)
    {
        if (in_range(val, kNecTimings.bitMark) || in_range(val, kNecTimings.bitOneSpace))
            return Result::SUCCESS;
        return Result::ERROR_FAILED_TO_RECEIVE;
    }

    bool parse_protocol(std::string_view text, Payload::Protocol& out)
    {
        if (text == "nec" || text == "NEC")
            out = Payload::Protocol::Nec;
        else if (text == "lgac" || text == "LGAC" || text == "lg_ac")
            out = Payload::Protocol::LgAc;
        else if (text == "raw" || text == "RAW")
            out = Payload::Protocol::Raw;
        else
            return false;
        return true;
    }

    uint32_t read_json_uint32(const ws::json& value)
    {
        if (value.is_number_unsigned())
            return value.get<uint32_t>();
        if (value.is_number_integer())
            return static_cast<uint32_t>(value.get<int64_t>());
        if (value.is_string())
        {
            uint32_t parsed = 0;
            if (parse_uint32(value.get<std::string>(), parsed))
                return parsed;
        }
        throw std::invalid_argument("invalid numeric json field");
    }

    uint8_t read_json_uint8(const ws::json& value)
    {
        return static_cast<uint8_t>(read_json_uint32(value));
    }

    Payload payload_from_json(const ws::json& entry)
    {
        if (!entry.is_object() || !entry.contains("protocol"))
            return {};

        const std::string protocolName = entry.at("protocol").get<std::string>();
        Payload::Protocol protocol = Payload::Protocol::None;
        if (!parse_protocol(protocolName, protocol))
            return {};

        switch (protocol) {
        case Payload::Protocol::Nec:
            if (entry.contains("repeat") && entry.at("repeat").get<bool>())
                return Payload::repeatCode();
            if (entry.contains("data"))
                return Payload(read_json_uint8(entry.at("code")), read_json_uint8(entry.at("data")));
            return Payload(read_json_uint8(entry.at("code")));
        case Payload::Protocol::LgAc:
            return Payload::fromRaw28(read_json_uint32(entry.at("value")));
        case Payload::Protocol::Raw: {
            const uint8_t bits = read_json_uint8(entry.at("bits"));
            return Payload::fromRawBits(bits, read_json_uint32(entry.at("value")));
        }
        default:
            return {};
        }
    }

    ws::json payload_to_json(const Payload& payload)
    {
        ws::json entry = ws::json::object();
        entry["protocol"] = Payload::protocolToString(payload.protocol());

        switch (payload.kind()) {
        case Payload::Kind::NecCodeOnly:
            entry["code"] = payload.code();
            break;
        case Payload::Kind::NecCodeData:
            entry["code"] = payload.code();
            entry["data"] = payload.data();
            break;
        case Payload::Kind::Repeat:
            entry["repeat"] = true;
            break;
        case Payload::Kind::LgAc28:
            entry["value"] = payload.raw28();
            break;
        default:
            if (Payload::isRawKind(payload.kind())) {
                entry["bits"] = payload.bitCount();
                entry["value"] = payload.rawBits();
            }
            break;
        }

        return entry;
    }

}  // namespace

const char* to_string(Result result)
{
    switch (result) {
    case Result::SUCCESS:
        return "SUCCESS";
    case Result::ERROR_INVALID_FORMAT:
        return "ERROR_INVALID_FORMAT";
    case Result::ERROR_INVALID_LENGTH:
        return "ERROR_INVALID_LENGTH";
    case Result::ERROR_FAILED_TO_OPEN_DEVICE:
        return "ERROR_FAILED_TO_OPEN_DEVICE";
    case Result::ERROR_NOT_INITIALIZED:
        return "ERROR_NOT_INITIALIZED";
    case Result::ERROR_CMD_ALREADY_EXISTS:
        return "ERROR_CMD_ALREADY_EXISTS";
    case Result::ERROR_CMD_NOT_FOUND:
        return "ERROR_CMD_NOT_FOUND";
    case Result::ERROR_CMD_EMPTY:
        return "ERROR_CMD_EMPTY";
    case Result::ERROR_CMD_DONT_MATCH:
        return "ERROR_CMD_DONT_MATCH";
    case Result::ERROR_INCOMPLETE_FRAME:
        return "ERROR_INCOMPLETE_FRAME";
    case Result::ERROR_FAILED_TO_RECEIVE:
        return "ERROR_FAILED_TO_RECEIVE";
    case Result::ERROR_FAILED_TO_TRANSMIT:
        return "ERROR_FAILED_TO_TRANSMIT";
    }
    return "UNKNOWN";
}

uint32_t Payload::maskBits(uint32_t value, uint8_t bitCount)
{
    if (bitCount >= 32)
        return value;
    if (bitCount == 0)
        return 0;
    return value & ((1U << bitCount) - 1U);
}

Payload Payload::repeatCode()
{
    Payload payload;
    payload.m_kind = Kind::Repeat;
    payload.m_value = 0;
    return payload;
}

Payload Payload::fromRaw28(uint32_t raw28)
{
    Payload payload;
    payload.m_kind = Kind::LgAc28;
    payload.m_value = maskBits(raw28, 28);
    return payload;
}

Payload Payload::fromRawBits(uint8_t bitCount, uint32_t bits)
{
    Payload payload;
    if (bitCount < 1 || bitCount > kMaxRawBits)
        return payload;

    payload.m_kind = static_cast<Kind>(bitCount);
    payload.m_value = maskBits(bits, bitCount);
    return payload;
}

bool Payload::isRawKind(Kind kind)
{
    const uint8_t value = static_cast<uint8_t>(kind);
    return value >= 1 && value <= kMaxRawBits;
}

const char* Payload::kindToString(Kind kind)
{
    switch (kind) {
    case Kind::Empty:
        return "Empty";
    case Kind::Repeat:
        return "Repeat";
    case Kind::NecCodeOnly:
        return "NecCodeOnly";
    case Kind::NecCodeData:
        return "NecCodeData";
    case Kind::LgAc28:
        return "LgAc28";
    default:
        break;
    }

    if (isRawKind(kind)) {
        static thread_local char buffer[8];
        std::snprintf(buffer, sizeof(buffer), "Raw%u", static_cast<uint8_t>(kind));
        return buffer;
    }

    return "Unknown";
}

const char* Payload::protocolToString(Protocol protocol)
{
    switch (protocol) {
    case Protocol::None:
        return "none";
    case Protocol::Raw:
        return "raw";
    case Protocol::Nec:
        return "nec";
    case Protocol::LgAc:
        return "lgac";
    }
    return "unknown";
}

Payload::Payload() = default;

Payload::Payload(uint8_t code)
    : m_kind(Kind::NecCodeOnly), m_value(code) {}

Payload::Payload(uint8_t code, uint8_t data)
    : m_kind(Kind::NecCodeData), m_value((static_cast<uint32_t>(data) << 8) | code) {}

Payload::Payload(std::string_view code)
{
    uint8_t parsed = 0;
    if (!parse_byte(code, parsed))
        return;
    m_kind = Kind::NecCodeOnly;
    m_value = parsed;
}

Payload::Payload(std::string_view code, std::string_view data)
{
    uint8_t parsedCode = 0;
    uint8_t parsedData = 0;
    if (!parse_byte(code, parsedCode) || !parse_byte(data, parsedData))
        return;
    m_kind = Kind::NecCodeData;
    m_value = (static_cast<uint32_t>(parsedData) << 8) | parsedCode;
}

Payload::Kind Payload::kind() const
{
    return m_kind;
}

Payload::Protocol Payload::protocol() const
{
    switch (m_kind) {
    case Kind::Repeat:
    case Kind::NecCodeOnly:
    case Kind::NecCodeData:
        return Protocol::Nec;
    case Kind::LgAc28:
        return Protocol::LgAc;
    case Kind::Empty:
        return Protocol::None;
    default:
        break;
    }

    if (isRawKind(m_kind))
        return Protocol::Raw;

    return Protocol::None;
}

uint8_t Payload::bitCount() const
{
    return bit_count_for_kind(m_kind);
}

uint8_t Payload::code() const
{
    return static_cast<uint8_t>(m_value & 0xFFu);
}

uint8_t Payload::data() const
{
    return static_cast<uint8_t>((m_value >> 8) & 0xFFu);
}

uint32_t Payload::rawBits() const
{
    if (isRawKind(m_kind) || m_kind == Kind::LgAc28)
        return maskBits(m_value, bitCount());
    return m_value;
}

uint32_t Payload::raw28() const
{
    return maskBits(m_value, 28);
}

uint32_t Payload::necWire32() const
{
    const uint8_t c = code();
    const uint8_t d = data();
    return static_cast<uint32_t>(c)
           | (static_cast<uint32_t>(static_cast<uint8_t>(~c)) << 8)
           | (static_cast<uint32_t>(d) << 16)
           | (static_cast<uint32_t>(static_cast<uint8_t>(~d)) << 24);
}

bool Payload::isNec() const
{
    return protocol() == Protocol::Nec && m_kind != Kind::Repeat;
}

bool Payload::isLgAc() const
{
    return m_kind == Kind::LgAc28;
}

bool Payload::isRaw() const
{
    return isRawKind(m_kind);
}

bool Payload::valid() const
{
    return m_kind != Kind::Empty;
}

bool Payload::matches(const Payload& other) const
{
    if (m_kind == Kind::NecCodeOnly && other.m_kind == Kind::NecCodeOnly)
        return code() == other.code();

    if (m_kind == Kind::NecCodeData && other.m_kind == Kind::NecCodeData)
        return code() == other.code() && data() == other.data();

    if (m_kind == Kind::LgAc28 && other.m_kind == Kind::LgAc28)
        return raw28() == other.raw28();

    if (isRawKind(m_kind) && m_kind == other.m_kind)
        return rawBits() == other.rawBits();

    return *this == other;
}

bool Payload::operator==(const Payload& other) const
{
    return m_kind == other.m_kind && m_value == other.m_value;
}

bool Payload::operator!=(const Payload& other) const
{
    return !(*this == other);
}

CommandList::CommandList() = default;

CommandList::~CommandList() = default;

Result CommandList::addCommand(std::string_view name, const Payload& payload, bool overwrite)
{
    if (!payload.valid() || payload.kind() == Payload::Kind::Empty)
        return Result::ERROR_CMD_EMPTY;

    if (!overwrite && m_commands.contains(std::string(name)))
        return Result::ERROR_CMD_ALREADY_EXISTS;

    m_commands[std::string(name)] = payload;
    return Result::SUCCESS;
}

Result CommandList::removeCommand(std::string_view name)
{
    return m_commands.erase(std::string(name)) > 0 ? Result::SUCCESS
                                                   : Result::ERROR_CMD_NOT_FOUND;
}

const Payload* CommandList::getPayload(std::string_view name) const
{
    auto it = m_commands.find(std::string(name));
    if (it != m_commands.end())
        return &it->second;
    return nullptr;
}

void CommandList::clear()
{
    m_commands.clear();
}

bool CommandList::loadFromFile(const std::string& filepath)
{
    std::ifstream in(filepath);
    if (!in.is_open())
        return false;

    ws::json root;
    try {
        in >> root;
    } catch (...) {
        return false;
    }

    if (!root.contains("commands") || !root.at("commands").is_object())
        return false;

    clear();
    for (const auto& [name, entry] : root.at("commands").items()) {
        Payload payload = payload_from_json(entry);
        if (payload.valid())
            addCommand(name, payload);
    }

    return true;
}

bool CommandList::saveToFile(const std::string& filepath) const
{
    ws::json root;
    root["commands"] = ws::json::object();

    for (const auto& [name, payload] : getSortedCommands())
        root["commands"][name] = payload_to_json(payload);

    std::ofstream out(filepath);
    if (!out.is_open())
        return false;

    out << root.dump(2) << '\n';
    return true;
}

std::vector<std::pair<std::string, Payload>> CommandList::getSortedCommands() const
{
    std::vector<std::pair<std::string, Payload>> sorted(m_commands.begin(), m_commands.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });
    return sorted;
}

Transmitter::Transmitter() = default;

Transmitter::Transmitter(std::shared_ptr<CommandList> cmd_list)
    : m_commandList(std::move(cmd_list)) {}

Transmitter::~Transmitter()
{
    shutdown();
}

void Transmitter::setCommandList(std::shared_ptr<CommandList> cmd_list)
{
    m_commandList = std::move(cmd_list);
}

const std::shared_ptr<CommandList>& Transmitter::getCommandList() const
{
    return m_commandList;
}

Result Transmitter::init(std::string_view dev)
{
    shutdown();

    const int fd = open(std::string(dev).c_str(), O_RDWR);
    if (fd < 0)
        return Result::ERROR_FAILED_TO_OPEN_DEVICE;

    m_fd = fd;
    return Result::SUCCESS;
}

void Transmitter::shutdown()
{
    if (m_fd >= 0) {
        close(m_fd);
        m_fd = -1;
    }
}

Result Transmitter::send(const Payload& payload) const
{
    if (m_fd < 0)
        return Result::ERROR_NOT_INITIALIZED;

    if (!payload.valid() || payload.kind() == Payload::Kind::Empty)
        return Result::ERROR_INVALID_FORMAT;

    if (payload.kind() == Payload::Kind::LgAc28 || Payload::isRawKind(payload.kind())) {
        const ProtocolTimings& timings =
            payload.kind() == Payload::Kind::LgAc28 ? kLgAcTimings : kNecTimings;
        const uint8_t bitCount = payload.bitCount();
        if (bitCount == 0)
            return Result::ERROR_INVALID_FORMAT;

        uint32_t rawBuf[2 + kMaxRawBits * 2 + 1];
        size_t idx = 0;
        rawBuf[idx++] = timings.headerMark;
        rawBuf[idx++] = timings.headerSpace;

        const uint32_t val = payload.rawBits();
        for (uint8_t bitIdx = 0; bitIdx < bitCount; ++bitIdx) {
            if ((val >> bitIdx) & 1U) {
                rawBuf[idx++] = timings.bitMark;
                rawBuf[idx++] = timings.bitOneSpace;
            } else {
                rawBuf[idx++] = timings.bitMark;
                rawBuf[idx++] = timings.bitZeroSpace;
            }
        }
        rawBuf[idx++] = timings.bitMark;
        return transmitRaw(rawBuf, idx);
    }

    if (payload.kind() == Payload::Kind::Repeat) {
        const uint32_t rawBuf[kNecRepeatPulses] = {
            kNecTimings.headerMark,
            kNecTimings.repeatSpace,
            kNecTimings.bitMark,
            kNecTimings.bitMark,
        };
        return transmitRaw(const_cast<uint32_t*>(rawBuf), kNecRepeatPulses);
    }

    uint32_t rawBuf[kNecFramePulses];
    size_t idx = 0;

    rawBuf[idx++] = kNecTimings.headerMark;
    rawBuf[idx++] = kNecTimings.headerSpace;

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
                rawBuf[idx++] = kNecTimings.bitMark;
                rawBuf[idx++] = kNecTimings.bitOneSpace;
            } else {
                rawBuf[idx++] = kNecTimings.bitMark;
                rawBuf[idx++] = kNecTimings.bitZeroSpace;
            }
        }
    }

    rawBuf[idx++] = kNecTimings.bitMark;
    return transmitRaw(rawBuf, idx);
}

Result Transmitter::send(std::string_view cmd_name) const
{
    if (!m_commandList)
        return Result::ERROR_CMD_NOT_FOUND;

    const Payload* payload = m_commandList->getPayload(cmd_name);
    if (!payload)
        return Result::ERROR_CMD_NOT_FOUND;

    if (payload->kind() == Payload::Kind::Empty || payload->kind() == Payload::Kind::Repeat)
        return Result::ERROR_CMD_DONT_MATCH;

    return send(*payload);
}

Result Transmitter::send(std::string_view cmd_name, uint8_t data) const
{
    if (!m_commandList)
        return Result::ERROR_CMD_NOT_FOUND;

    const Payload* registered = m_commandList->getPayload(cmd_name);
    if (!registered)
        return Result::ERROR_CMD_NOT_FOUND;

    if (registered->kind() != Payload::Kind::NecCodeOnly)
        return Result::ERROR_CMD_DONT_MATCH;

    return send(Payload(registered->code(), data));
}

Result Transmitter::send(std::string_view cmd_name, std::string_view data_str) const
{
    uint8_t data = 0;
    if (!parse_byte(data_str, data))
        return Result::ERROR_INVALID_FORMAT;

    return send(cmd_name, data);
}

Result Transmitter::transmitRaw(uint32_t* buf, size_t count) const
{
    if (m_fd < 0)
        return Result::ERROR_NOT_INITIALIZED;

    if (write(m_fd, buf, count * sizeof(uint32_t)) < 0)
        return Result::ERROR_FAILED_TO_TRANSMIT;

    return Result::SUCCESS;
}

Receiver::Receiver() = default;

Receiver::Receiver(std::shared_ptr<CommandList> cmd_list)
    : m_commandList(std::move(cmd_list)) {}

Receiver::~Receiver()
{
    shutdown();
}

void Receiver::setCommandList(std::shared_ptr<CommandList> cmd_list)
{
    m_commandList = std::move(cmd_list);
}

const std::shared_ptr<CommandList>& Receiver::getCommandList() const
{
    return m_commandList;
}

Result Receiver::init(std::string_view dev)
{
    shutdown();

    const int fd = open(std::string(dev).c_str(), O_RDONLY);
    if (fd < 0)
        return Result::ERROR_FAILED_TO_OPEN_DEVICE;

    m_fd = fd;
    m_running = true;
    m_thread = std::thread(&Receiver::recvThreadFunc, this);
    return Result::SUCCESS;
}

void Receiver::shutdown()
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

bool Receiver::hasData() const
{
    std::shared_lock lock(m_mutex);
    Payload payload;
    size_t consumed = 0;
    return tryParseFrame(payload, consumed);
}

void Receiver::appendLircPackets(const uint32_t* raw, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        const uint32_t packet = raw[i];
        const uint32_t mode = packet & kLircMode2Mask;

        if (mode == kLircMode2Timeout) {
            if (m_pendingMark.has_value()) {
                m_buffer.push_back(*m_pendingMark);
                m_buffer.push_back(packet & kLircValueMask);
                m_pendingMark.reset();
            }
            continue;
        }

        if (mode == kLircMode2Frequency || mode == kLircMode2Overflow)
            continue;

        const uint32_t duration = packet & kLircValueMask;
        if (duration == 0)
            continue;

        if (packet & kLircPulseBit) {
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

void Receiver::recvThreadFunc()
{
    uint32_t readBuf[128];
    struct pollfd pfd;
    pfd.fd = m_fd;
    pfd.events = POLLIN;

    while (m_running) {
        if (m_fd < 0)
            break;

        int pollRes = poll(&pfd, 1, 100);
        if (pollRes < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (pollRes == 0)
            continue;

        const ssize_t bytesRead = read(m_fd, readBuf, sizeof(readBuf));
        if (bytesRead <= 0)
            continue;

        const size_t packetCount = static_cast<size_t>(bytesRead) / sizeof(uint32_t);
        if (packetCount == 0)
            continue;

        std::unique_lock lock(m_mutex);
        appendLircPackets(readBuf, packetCount);
    }
}

bool Receiver::tryParseFrame(Payload& out_payload, size_t& consumed) const
{
    consumed = 0;

    for (size_t start = 0; start + 3 <= m_buffer.size(); ++start) {
        if (!in_range(m_buffer[start], kLgAcTimings.headerMark, kLgAcTimings.headerMargin)
            || !in_range(m_buffer[start + 1], kNecTimings.repeatSpace, 350)
            || !in_range(m_buffer[start + 2], kNecTimings.bitMark)) {
            continue;
        }

        out_payload = Payload::repeatCode();
        if (start + 4 <= m_buffer.size() && in_range(m_buffer[start + 3], kNecTimings.bitMark))
            consumed = start + 4;
        else
            consumed = start + 3;
        return true;
    }

    for (size_t start = 0; start + 2 <= m_buffer.size(); ++start) {
        if (!in_range(m_buffer[start], kLgAcTimings.headerMark, kLgAcTimings.headerMargin)
            || !in_range(m_buffer[start + 1], kLgAcTimings.headerSpace, kLgAcTimings.headerMargin)) {
            continue;
        }

        uint32_t payloadBits = 0;
        size_t idx = start + 2;
        int bitsParsed = 0;

        while (idx + 1 < m_buffer.size() && bitsParsed < static_cast<int>(kMaxRawBits)) {
            const uint32_t mark = m_buffer[idx];
            const uint32_t space = m_buffer[idx + 1];

            if (in_bit_range(mark) != Result::SUCCESS
                || in_bit_range(space) != Result::SUCCESS) {
                break;
            }

            if (space > (kNecTimings.bitMark + kNecTimings.bitOneSpace) / 2)
                payloadBits |= (1U << bitsParsed);

            bitsParsed++;
            idx += 2;
        }

        if (bitsParsed < 1 || bitsParsed > static_cast<int>(kMaxRawBits))
            continue;

        if (idx >= m_buffer.size() || !in_range(m_buffer[idx], kNecTimings.bitMark))
            continue;

        if (bitsParsed == 32) {
            const uint8_t code = static_cast<uint8_t>(payloadBits & 0xFFu);
            const uint8_t codeInv = static_cast<uint8_t>((payloadBits >> 8) & 0xFFu);
            const uint8_t data = static_cast<uint8_t>((payloadBits >> 16) & 0xFFu);
            const uint8_t dataInv = static_cast<uint8_t>((payloadBits >> 24) & 0xFFu);

            if ((code ^ codeInv) == 0xFF && (data ^ dataInv) == 0xFF) {
                if (data == 0)
                    out_payload = Payload(code);
                else
                    out_payload = Payload(code, data);
            } else {
                out_payload = Payload::fromRawBits(32, payloadBits);
            }
        } else if (bitsParsed == 28) {
            out_payload = Payload::fromRaw28(payloadBits);
        } else {
            out_payload = Payload::fromRawBits(static_cast<uint8_t>(bitsParsed), payloadBits);
        }

        consumed = idx + 1;
        return true;
    }

    return false;
}

Result Receiver::recv(Payload& out_payload)
{
    std::unique_lock lock(m_mutex);

    size_t consumed = 0;
    if (!tryParseFrame(out_payload, consumed))
        return Result::ERROR_INCOMPLETE_FRAME;

    m_buffer.erase(m_buffer.begin(), m_buffer.begin() + static_cast<std::ptrdiff_t>(consumed));
    return Result::SUCCESS;
}

Result Receiver::recv(Payload& out_payload, std::string& out_cmd_name)
{
    PROPAGATE_ERROR(recv(out_payload));

    if (!m_commandList)
        return Result::ERROR_CMD_NOT_FOUND;

    for (const auto& [name, registered] : *m_commandList) {
        if (registered.matches(out_payload)) {
            out_cmd_name = name;
            return Result::SUCCESS;
        }
    }

    return Result::ERROR_CMD_NOT_FOUND;
}

IR_NAMESPACE_END
