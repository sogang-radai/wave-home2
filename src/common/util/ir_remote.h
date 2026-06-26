#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

enum class IRResult
{
    SUCCESS = 0,
    ERROR_INVALID_FORMAT,
    ERROR_INVALID_LENGTH,
    ERROR_FAILED_TO_OPEN_DEVICE,
    ERROR_NOT_INITIALIZED,
    ERROR_CMD_ALREADY_EXISTS,
    ERROR_CMD_NOT_FOUND,
    ERROR_FAILED_TO_RECEIVE,
    ERROR_FAILED_TO_TRANSMIT,
};

struct IRPayload
{
    IRPayload();
    IRPayload(uint8_t code, uint8_t data);

    uint8_t code;
    uint8_t data;
};

class IRRemote
{
public:
    IRRemote();
    IRRemote(const IRRemote&) = delete;
    IRRemote& operator=(const IRRemote&) = delete;
    ~IRRemote();

    IRResult init(const char* dev);

    IRResult addCommand(const std::string& name, const IRPayload& payload);
    IRResult addCommand(const std::string& name, const char* c, const char* d);
    IRResult removeCommand(const std::string& name);
    
    IRResult getCommand(const std::string& name, IRPayload& out_payload) const;
    const std::unordered_map<std::string, IRPayload>& enumerateCommand() const;

    IRResult send(const char* cmd_name) const;
    IRResult send(const char* cmd_name, uint8_t data) const;
    IRResult send(const char* cmd_name, const char* data_str) const;
    IRResult send(const IRPayload& cmd) const;
    IRResult sendRepeat() const;

    IRResult recv(IRPayload& out_payload);
    IRResult recv(IRPayload& out_payload, std::string& out_cmd_name);

private:
    IRResult transmitRaw(uint32_t* buf, size_t count) const;

private:
    std::unordered_map<std::string, IRPayload> m_commands;
    int m_fd;
};
