#include "ir_remote.h"

#include <fcntl.h>
#include <unistd.h>

#define PROPAGATE_ERROR(res) \
    do { \
        if (res != IRResult::SUCCESS) { \
            return res; \
        } \
    } while (0)

namespace 
{
    constexpr uint32_t NEC_HEADER_MARK_US = 9000;
    constexpr uint32_t NEC_HEADER_SPACE_US = 4500;
    constexpr uint32_t NEC_REPEAT_SPACE_US = 2250;
    constexpr uint32_t NEC_MARK_US = 560;
    constexpr uint32_t NEC_SPACE_US = 1690;

    IRResult bin_to_u8(const char* str, uint8_t& out_val)
    {
        int i = 0;
        uint8_t res = 0;
        out_val = 0;

        for (; i < 8 && *str; i++, str++)
        {
            if (*str != '0' && *str != '1')
                return IRResult::ERROR_INVALID_FORMAT;
            res = (res << 1) | (*str - '0');
        }

        if (i != 8 || *str != '\0')
            return IRResult::ERROR_INVALID_LENGTH;

        out_val = res;
        return IRResult::SUCCESS;
    };

    IRResult in_bit_range(uint32_t val)
    {
        constexpr uint32_t margin = 200;

        if (NEC_MARK_US - margin <= val && val <= NEC_MARK_US + margin)
            return IRResult::SUCCESS;
        if (NEC_SPACE_US - margin <= val && val <= NEC_SPACE_US + margin)
            return IRResult::SUCCESS;

        return IRResult::ERROR_FAILED_TO_RECEIVE;
    }
}

// ============================================================================
// IRPayload
// ============================================================================

IRPayload::IRPayload() :
    code(0),
    data(0) {}

IRPayload::IRPayload(uint8_t code, uint8_t data) :
    code(code),
    data(data) {}

// ============================================================================
// IRRemote
// ============================================================================

IRRemote::IRRemote() :
    m_fd(-1) {}

IRRemote::~IRRemote()
{
    if (m_fd >= 0)
        close(m_fd);
}

IRResult IRRemote::init(const char* dev)
{
    if (m_fd >= 0)
    {
        close(m_fd);
        m_commands.clear();
    }

    if ((m_fd = open(dev, O_RDWR)) < 0)
        return IRResult::ERROR_FAILED_TO_OPEN_DEVICE;

    return IRResult::SUCCESS;
}

IRResult IRRemote::addCommand(const std::string& name, const IRPayload& payload)
{
    if (m_commands.find(name) != m_commands.end())
        return IRResult::ERROR_CMD_ALREADY_EXISTS;

    m_commands.insert({ name, payload });

    return IRResult::SUCCESS;
}

IRResult IRRemote::addCommand(const std::string& name, const char* c, const char* d)
{
    uint8_t code = 0;
    uint8_t data = 0;

    PROPAGATE_ERROR(bin_to_u8(c, code));
    PROPAGATE_ERROR(bin_to_u8(d, data));

    return addCommand(name, IRPayload(code, data));
}

IRResult IRRemote::removeCommand(const std::string& name)
{
    return m_commands.erase(name) > 0 ? IRResult::SUCCESS : IRResult::ERROR_CMD_NOT_FOUND;
}

IRResult IRRemote::getCommand(const std::string& name, IRPayload& out_payload) const
{
    if (auto it = m_commands.find(name); it != m_commands.end())
    {
        out_payload = it->second;
        return IRResult::SUCCESS;
    }

    return IRResult::ERROR_CMD_NOT_FOUND;
}

const std::unordered_map<std::string, IRPayload>& IRRemote::enumerateCommand() const
{
    return m_commands;
}

IRResult IRRemote::send(const char* cmd_name) const
{
    if (auto it = m_commands.find(cmd_name); it != m_commands.end())
        return send(it->second);

    return IRResult::ERROR_CMD_NOT_FOUND;
}

IRResult IRRemote::send(const char* cmd_name, uint8_t data) const
{
    if (auto it = m_commands.find(cmd_name); it != m_commands.end())
        return send(IRPayload(it->second.code, data));

    return IRResult::ERROR_CMD_NOT_FOUND;
}

IRResult IRRemote::send(const char* cmd_name, const char* data_str) const
{
    uint8_t data = 0;
    PROPAGATE_ERROR(bin_to_u8(data_str, data));

    return send(cmd_name, data);
}

IRResult IRRemote::send(const IRPayload& payload) const
{
    uint32_t raw_buf[67];
    uint32_t idx = 0;

    raw_buf[idx++] = NEC_HEADER_MARK_US;
    raw_buf[idx++] = NEC_HEADER_SPACE_US;

    uint8_t payload_bytes[4] = {
        payload.code,
        (uint8_t)~payload.code,
        payload.data,
        (uint8_t)~payload.data
    };

    for (int byte_idx = 0; byte_idx < 4; byte_idx++)
    {
        uint8_t current_byte = payload_bytes[byte_idx];
        for (int bit_idx = 0; bit_idx < 8; bit_idx++)
        {
            if ((current_byte >> bit_idx) & 1)
            {
                raw_buf[idx++] = NEC_MARK_US;
                raw_buf[idx++] = NEC_SPACE_US;
            }
            else
            {
                raw_buf[idx++] = NEC_MARK_US;
                raw_buf[idx++] = NEC_MARK_US;
            }
        }
    }

    raw_buf[idx++] = NEC_MARK_US;

    return transmitRaw(raw_buf, idx);
}

IRResult IRRemote::sendRepeat() const
{
    uint32_t raw_buf[4] = { NEC_HEADER_MARK_US, NEC_REPEAT_SPACE_US, NEC_MARK_US, NEC_MARK_US };
    return transmitRaw(raw_buf, 4);
}

IRResult IRRemote::recv(IRPayload& out_payload)
{
    if (m_fd < 0)
        return IRResult::ERROR_NOT_INITIALIZED;

    uint32_t buf[128];
    ssize_t bytes_read = read(m_fd, buf, sizeof(buf));

    if (bytes_read < 0)
        return IRResult::ERROR_FAILED_TO_RECEIVE;

    if (bytes_read / sizeof(uint32_t) < 67)
        return IRResult::ERROR_INVALID_LENGTH;

    uint32_t payload = 0;
    size_t idx = 2;

    for (int i = 0; i < 32; i++, idx += 2)
    {
        uint32_t mark = buf[idx];
        uint32_t space = buf[idx + 1];

        PROPAGATE_ERROR(in_bit_range(mark));
        PROPAGATE_ERROR(in_bit_range(space));

        if (space > (NEC_MARK_US + NEC_SPACE_US) / 2)
            payload |= (1U << i);
    }

    uint8_t code = payload & 0xFF;
    uint8_t code_inv = (payload >> 8) & 0xFF;
    uint8_t data = (payload >> 16) & 0xFF;
    uint8_t data_inv = (payload >> 24) & 0xFF;

    if ((code ^ code_inv) != 0xFF || (data ^ data_inv) != 0xFF)
        return IRResult::ERROR_FAILED_TO_RECEIVE;

    out_payload.code = code;
    out_payload.data = data;

    return IRResult::SUCCESS;
}

IRResult IRRemote::recv(IRPayload& out_payload, std::string& out_cmd_name)
{
    PROPAGATE_ERROR(recv(out_payload));

    for (const auto& pair : m_commands)
    {
        if (pair.second.code == out_payload.code && pair.second.data == out_payload.data)
        {
            out_cmd_name = pair.first;
            return IRResult::SUCCESS;
        }
    }

    return IRResult::ERROR_CMD_NOT_FOUND;
}

IRResult IRRemote::transmitRaw(uint32_t* buf, size_t count) const
{
    if (m_fd < 0)
        return IRResult::ERROR_NOT_INITIALIZED;
    if (write(m_fd, buf, count * sizeof(uint32_t)) < 0)
        return IRResult::ERROR_FAILED_TO_TRANSMIT;

    return IRResult::SUCCESS;
}