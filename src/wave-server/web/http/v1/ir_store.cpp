#include "ir_store.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <sstream>

#include "../../../core/logger.h"
#include "util/time_util.h"
#include "../../../device/platform/radai_ws.h"

WAVE_NAMESPACE_BEGIN
WEB_NAMESPACE_BEGIN
namespace v1 {

namespace
{

    std::string slugify(const std::string& input)
    {
    std::string out;
    out.reserve(input.size());
    bool prev_sep = true;
    for (unsigned char ch : input)
    {
        if (std::isalnum(ch))
        {
            out.push_back(static_cast<char>(std::tolower(ch)));
            prev_sep = false;
        }
        else if (!prev_sep)
        {
            out.push_back('_');
            prev_sep = true;
        }
    }
    while (!out.empty() && out.back() == '_')
        out.pop_back();
    return out.empty() ? "command" : out;
    }

    } // namespace

std::string IrStore::iso_now_kst()
{
    const auto now = formatTimestamp();
    if (now.size() >= 19)
        return now.substr(0, 10) + "T" + now.substr(11, 8) + "+09:00";
    return now;
}

std::string IrStore::make_command_id(const std::string& name)
{
    const auto base = slugify(name);
    const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return "ir_" + base + "_" + std::to_string(stamp % 100000);
}

IrStore::Command IrStore::command_from_json(const Json::Value& value)
{
    Command command;
    if (value.isMember("id") && value["id"].isString())
        command.id = value["id"].asString();
    if (value.isMember("name"))
        command.name = value["name"].asString();
    if (value.isMember("description"))
        command.description = value["description"].asString();
    if (value.isMember("deviceHint"))
        command.deviceHint = value["deviceHint"].asString();
    if (value.isMember("unit"))
        command.unit = value["unit"].asString();
    if (value.isMember("source"))
        command.source = value["source"].asString();
    if (value.isMember("createdAt"))
        command.createdAt = value["createdAt"].asString();
    if (value.isMember("timings") && value["timings"].isArray())
    {
        for (const auto& item : value["timings"])
        {
            if (item.isUInt() || item.isInt())
                command.timings.push_back(static_cast<uint16_t>(item.asUInt()));
        }
    }
    return command;
}

Json::Value IrStore::command_to_json(const Command& command, bool include_timings)
{
    Json::Value out;
    out["id"] = command.id;
    out["name"] = command.name;
    if (!command.description.empty())
        out["description"] = command.description;
    if (!command.deviceHint.empty())
        out["deviceHint"] = command.deviceHint;
    out["unit"] = command.unit.empty() ? "us" : command.unit;
    out["source"] = command.source.empty() ? "manual" : command.source;
    if (!command.createdAt.empty())
        out["createdAt"] = command.createdAt;
    if (include_timings)
    {
        Json::Value timings(Json::arrayValue);
        for (const auto timing : command.timings)
            timings.append(static_cast<Json::UInt>(timing));
        out["timings"] = timings;
    }
    return out;
}

bool IrStore::load(const std::filesystem::path& path, std::string& out_error)
{
    std::lock_guard lock(m_mutex);
    m_path = path;
    m_commands.clear();

    if (!std::filesystem::exists(path))
    {
        out_error = "ir list not found: " + path.string();
        return false;
    }

    std::ifstream in(path);
    if (!in)
    {
        out_error = "failed to open ir list: " + path.string();
        return false;
    }

    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errors;
    if (!Json::parseFromStream(builder, in, &root, &errors))
    {
        out_error = "invalid ir list json: " + errors;
        return false;
    }

    if (!root.isMember("commands") || !root["commands"].isArray())
    {
        out_error = "ir list missing commands array";
        return false;
    }

    for (const auto& item : root["commands"])
        m_commands.push_back(command_from_json(item));

    out_error.clear();
    return true;
}

const IrStore::Command* IrStore::findLocked(const std::string& command_id) const
{
    const auto it = std::find_if(
        m_commands.begin(),
        m_commands.end(),
        [&](const Command& command) { return command.id == command_id; });
    return it != m_commands.end() ? &(*it) : nullptr;
}

bool IrStore::persistLocked(std::string& code) const
{
    if (m_path.empty())
    {
        code = "IR_STORE_UNAVAILABLE";
        return false;
    }

    Json::Value root;
    Json::Value commands(Json::arrayValue);
    for (const auto& command : m_commands)
        commands.append(command_to_json(command, true));
    root["commands"] = commands;

    const auto tmp_path = m_path.string() + ".tmp";
    {
        std::ofstream out(tmp_path, std::ios::trunc);
        if (!out)
        {
            code = "IR_SAVE_FAILED";
            return false;
        }
        out << root.toStyledString();
    }

    std::error_code ec;
    std::filesystem::rename(tmp_path, m_path, ec);
    if (ec)
    {
        std::filesystem::remove(tmp_path, ec);
        code = "IR_SAVE_FAILED";
        return false;
    }

    code.clear();
    return true;
}

Json::Value IrStore::listCommands() const
{
    std::lock_guard lock(m_mutex);
    Json::Value items(Json::arrayValue);
    for (const auto& command : m_commands)
        items.append(command_to_json(command, true));
    return items;
}

Json::Value IrStore::getCommand(const std::string& command_id, std::string& code) const
{
    std::lock_guard lock(m_mutex);
    const auto* command = findLocked(command_id);
    if (!command)
    {
        code = "NOT_FOUND";
        return Json::Value();
    }

    code.clear();
    return command_to_json(*command, true);
}

Json::Value IrStore::saveCommand(const Json::Value& body, std::string& code)
{
    if (!body.isMember("name") || !body["name"].isString() || body["name"].asString().empty())
    {
        code = "INVALID_BODY";
        return Json::Value();
    }

    if (!body.isMember("timings") || !body["timings"].isArray() || body["timings"].empty())
    {
        code = "INVALID_TIMINGS";
        return Json::Value();
    }

    std::lock_guard lock(m_mutex);
    auto command = command_from_json(body);
    if (command.timings.empty())
    {
        code = "INVALID_TIMINGS";
        return Json::Value();
    }

    const bool has_id = !command.id.empty();
    const bool is_update = has_id && findLocked(command.id) != nullptr;
    if (has_id && !is_update)
    {
        code = "NOT_FOUND";
        return Json::Value();
    }

    if (!is_update)
    {
        if (command.id.empty())
            command.id = make_command_id(command.name);
        if (command.createdAt.empty())
            command.createdAt = iso_now_kst();
        if (command.source.empty())
            command.source = "manual";
        m_commands.push_back(std::move(command));
    }
    else
    {
        auto it = std::find_if(
            m_commands.begin(),
            m_commands.end(),
            [&](const Command& existing) { return existing.id == command.id; });
        if (it == m_commands.end())
        {
            code = "NOT_FOUND";
            return Json::Value();
        }
        it->name = command.name;
        it->description = command.description;
        it->deviceHint = command.deviceHint;
        it->timings = command.timings;
        if (!command.source.empty())
            it->source = command.source;
        command = *it;
    }

    if (!persistLocked(code))
        return Json::Value();

    code.clear();
    return command_to_json(command, true);
}

Json::Value IrStore::deleteCommand(const std::string& command_id, std::string& code)
{
    std::lock_guard lock(m_mutex);
    const auto it = std::find_if(
        m_commands.begin(),
        m_commands.end(),
        [&](const Command& command) { return command.id == command_id; });
    if (it == m_commands.end())
    {
        code = "NOT_FOUND";
        return Json::Value();
    }

    m_commands.erase(it);
    if (!persistLocked(code))
        return Json::Value();

    Json::Value body;
    body["id"] = command_id;
    code.clear();
    return body;
}

Json::Value IrStore::learnFromDevice(dev::RadaiWs& wave_station, uint32_t timeout_ms, std::string& code)
{
    dev::IrTimingFrame frame;
    if (!wave_station.waitForIr(frame, timeout_ms) || frame.timingsUs.empty())
    {
        code = "IR_LEARN_TIMEOUT";
        return Json::Value();
    }

    Json::Value timings(Json::arrayValue);
    for (const auto timing : frame.timingsUs)
        timings.append(static_cast<Json::UInt>(timing));

    Json::Value body;
    body["timings"] = timings;
    code.clear();
    return body;
}

} // namespace v1
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
