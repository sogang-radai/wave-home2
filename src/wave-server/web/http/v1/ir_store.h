#pragma once

#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include <json/json.h>

#include "core/coredefs.h"

WAVE_NAMESPACE_BEGIN
DEVICE_NAMESPACE_BEGIN
class RadaiWs;
DEVICE_NAMESPACE_END
WEB_NAMESPACE_BEGIN
namespace v1 {

class IrStore
{
public:
    bool load(const std::filesystem::path& path, std::string& out_error);

    Json::Value listCommands() const;
    Json::Value getCommand(const std::string& command_id, std::string& code) const;
    Json::Value saveCommand(const Json::Value& body, std::string& code);
    Json::Value deleteCommand(const std::string& command_id, std::string& code);

    Json::Value learnFromDevice(
        dev::RadaiWs& wave_station,
        uint32_t timeout_ms,
        std::string& code);

private:
    struct Command
    {
        std::string id;
        std::string name;
        std::string description;
        std::string deviceHint;
        std::string unit = "us";
        std::string source = "manual";
        std::string createdAt;
        std::vector<uint16_t> timings;
    };

    static Command command_from_json(const Json::Value& value);
    static Json::Value command_to_json(const Command& command, bool include_timings);
    static std::string iso_now_kst();
    static std::string make_command_id(const std::string& name);

    bool persistLocked(std::string& code) const;
    const Command* findLocked(const std::string& command_id) const;

    mutable std::mutex m_mutex;
    std::filesystem::path m_path;
    std::vector<Command> m_commands;
};

} // namespace v1
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
