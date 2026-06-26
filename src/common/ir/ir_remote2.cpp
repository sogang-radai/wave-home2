#include "ir_remote2.h"

#include <fcntl.h>
#include <unistd.h>
#include <cstring>

#define PROPAGATE_ERROR(res) \
    do { \
        if (res != IRResult::SUCCESS) { \
            return res; \
        } \
    } while (0)

namespace 
{
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
        constexpr uint32_t bit0 = 560;
        constexpr uint32_t bit1 = 1690;
        constexpr uint32_t margin = 200;

        if (bit0 - margin <= val && val <= bit0 + margin)
            return IRResult::SUCCESS;
        if (bit1 - margin <= val && val <= bit1 + margin)
            return IRResult::SUCCESS;

        return IRResult::ERROR_FAILED_TO_RECEIVE;
    }
}

// ============================================================================
// IRCommand
// ============================================================================

IRCommand::IRCommand() :
    code(0),
    data(0) {}

IRCommand::IRCommand(uint8_t c, uint8_t d) :
    code(c),
    data(d) {}

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

IRResult IRRemote::addCommand(const std::string& name, const IRCommand& cmd)
{
    if (m_commands.find(name) != m_commands.end())
        return IRResult::ERROR_CMD_ALREADY_EXISTS;

    m_commands[name] = cmd;

    return IRResult::SUCCESS;
}

IRResult IRRemote::addCommand(const std::string& name, const char* c, const char* d)
{
    uint8_t code = 0;
    uint8_t data = 0;

    PROPAGATE_ERROR(bin_to_u8(c, code));
    PROPAGATE_ERROR(bin_to_u8(d, data));

    return addCommand(name, IRCommand(code, data));
}

IRResult IRRemote::removeCommand(const std::string& name)
{
    return m_commands.erase(name) > 0 ? IRResult::SUCCESS : IRResult::ERROR_CMD_NOT_FOUND;
}

IRResult IRRemote::getCommand(const std::string& name, IRCommand& out_cmd) const
{
    if (auto it = m_commands.find(name); it != m_commands.end())
    {
        out_cmd = it->second;
        return IRResult::SUCCESS;
    }

    return IRResult::ERROR_CMD_NOT_FOUND;
}

const std::unordered_map<std::string, IRCommand>& IRRemote::enumerateCommand() const
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
        return send(IRCommand(it->second.code, data));

    return IRResult::ERROR_CMD_NOT_FOUND;
}

IRResult IRRemote::send(const char* cmd_name, const char* data_str) const
{
    uint8_t data = 0;
    PROPAGATE_ERROR(bin_to_u8(data_str, data));

    return send(cmd_name, data);
}

IRResult IRRemote::send(const IRCommand& cmd) const
{
    uint32_t raw_buf[66];
    uint32_t idx = 0;

    raw_buf[idx++] = 9000;
    raw_buf[idx++] = 4500;

    uint32_t payload = 0;
    payload |= (cmd.code);
    payload |= ((uint8_t)~cmd.code) << 8;
    payload |= (cmd.data) << 16;
    payload |= ((uint8_t)~cmd.data) << 24;

    for (int i = 31; i >= 0; i--)
    {
        if ((payload >> i) & 1)
        {
            raw_buf[idx++] = 560;
            raw_buf[idx++] = 1690;
        }
        else
        {
            raw_buf[idx++] = 560;
            raw_buf[idx++] = 560;
        }
    }

    raw_buf[idx++] = 560;

    return transmitRaw(raw_buf, idx);
}

IRResult IRRemote::sendRepeat() const
{
    uint32_t raw_buf[4] = { 9000, 2250, 560, 560 };
    return transmitRaw(raw_buf, 4);
}

IRResult IRRemote::recv(IRCommand& out_cmd)
{
    if (m_fd < 0)
        return IRResult::ERROR_NOT_INITIALIZED;

    uint32_t buf[128];
    ssize_t bytes_read = read(m_fd, buf, sizeof(buf));

    if (bytes_read < 0)
        return IRResult::ERROR_FAILED_TO_RECEIVE;

    if (bytes_read / sizeof(uint32_t) < 66)
        return IRResult::ERROR_INVALID_LENGTH;

    uint32_t payload = 0;
    size_t idx = 2;

    for (int i = 31; i >= 0; i--, idx += 2)
    {
        uint32_t mark = buf[idx];
        uint32_t space = buf[idx + 1];

        PROPAGATE_ERROR(in_bit_range(mark));
        PROPAGATE_ERROR(in_bit_range(space));

        if (space > 1000)
            payload |= (1 << i);
    }

    uint8_t code = payload & 0xFF;
    uint8_t code_inv = (payload >> 8) & 0xFF;
    uint8_t data = (payload >> 16) & 0xFF;
    uint8_t data_inv = (payload >> 24) & 0xFF;

    if ((code ^ code_inv) != 0xFF || (data ^ data_inv) != 0xFF)
        return IRResult::ERROR_FAILED_TO_RECEIVE;

    out_cmd.code = code;
    out_cmd.data = data;

    return IRResult::SUCCESS;
}

IRResult IRRemote::recv(IRCommand& out_cmd, std::string& out_cmd_name)
{
    PROPAGATE_ERROR(recv(out_cmd));

    for (const auto& pair : m_commands)
    {
        if (pair.second.code == out_cmd.code && pair.second.data == out_cmd.data)
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