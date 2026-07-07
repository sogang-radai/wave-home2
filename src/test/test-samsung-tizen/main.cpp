#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "core/json.h"
#include "device/platform/samsung_tizen.h"
#include "util/arg_parser.h"

using ws::json;
using namespace ws::dev;

namespace
{
    json loadDeviceList(const std::string& path)
    {
        std::ifstream in(path);
        if (!in.is_open())
            throw std::runtime_error("failed to open " + path);

        json root;
        in >> root;
        return root;
    }

    void saveDeviceList(const std::string& path, const json& root)
    {
        std::ofstream out(path);
        if (!out.is_open())
            throw std::runtime_error("failed to write " + path);
        out << root.dump(4) << '\n';
    }

    json findSamsungConfig(const json& root)
    {
        for (const auto& device : root.at("device_list"))
        {
            if (device.at("class").get<std::string>() == "samsung_g7")
                return device;
        }
        throw std::runtime_error("samsung_g7 device not found");
    }

    json& findSamsungEntry(json& root)
    {
        for (auto& device : root.at("device_list"))
        {
            if (device.at("class").get<std::string>() == "samsung_g7")
                return device;
        }
        throw std::runtime_error("samsung_g7 device not found");
    }

    void printResult(const json& result)
    {
        if (result.contains("code"))
        {
            std::cout << "  code " << result["code"].get<int>();
            if (result.contains("message"))
                std::cout << " (" << result["message"].get<std::string>() << ")";
            std::cout << '\n';
        }
        else
        {
            std::cout << "  " << result.dump() << '\n';
        }
    }

    void printInvoke(const std::string& name, int code)
    {
        std::cout << "  " << name << " => " << (code == 0 ? "ok" : "fail") << " (" << code << ")\n";
    }

    void printHelp()
    {
        std::cout <<
            "commands:\n"
            "  gc gs gin gi gss          get caps / state / inputs / input / session\n"
            "  vu vd m                   volume up / down / mute\n"
            "  cu cd                     channel up / down\n"
            "  i <hdmi1|hdmi2|dp|...>    switch input\n"
            "  k <KEY_...>               raw remote key\n"
            "  h q                       help / quit\n";
    }

    bool runQuery(SamsungTizen& tv, std::string_view name)
    {
        printResult(tv.query(name, json::object()));
        return true;
    }

    bool runCommand(SamsungTizen& tv, const std::string& cmd, std::istringstream& in)
    {
        if (cmd == "q" || cmd == "quit" || cmd == "exit")
            return false;
        if (cmd == "h" || cmd == "help" || cmd == "?")
        {
            printHelp();
            return true;
        }
        if (cmd == "gc")
            return runQuery(tv, "capabilities");
        if (cmd == "gs")
            return runQuery(tv, "state");
        if (cmd == "gin")
            return runQuery(tv, "inputs");
        if (cmd == "gi")
            return runQuery(tv, "input");
        if (cmd == "gss")
            return runQuery(tv, "session");
        if (cmd == "vu" || cmd == "volume_up")
        {
            printInvoke("volume_up", tv.invoke("volume_up", json::object()));
            return true;
        }
        if (cmd == "vd" || cmd == "volume_down")
        {
            printInvoke("volume_down", tv.invoke("volume_down", json::object()));
            return true;
        }
        if (cmd == "m" || cmd == "mute")
        {
            printInvoke("mute", tv.invoke("mute", json::object()));
            return true;
        }
        if (cmd == "cu" || cmd == "channel_up")
        {
            printInvoke("channel_up", tv.invoke("channel_up", json::object()));
            return true;
        }
        if (cmd == "cd" || cmd == "channel_down")
        {
            printInvoke("channel_down", tv.invoke("channel_down", json::object()));
            return true;
        }
        if (cmd == "i" || cmd == "input")
        {
            std::string source;
            in >> source;
            if (source.empty())
            {
                std::cout << "  usage: i <hdmi1|hdmi2|dp|...>\n";
                return true;
            }
            if (source == "dp")
                source = "displayport";
            printInvoke("input", tv.invoke("input", {{"source", source}}));
            return true;
        }
        if (cmd == "k" || cmd == "key")
        {
            std::string key;
            in >> key;
            if (key.empty())
            {
                std::cout << "  usage: k KEY_VOLUP\n";
                return true;
            }
            printInvoke("send_key", tv.invoke("send_key", {{"key", key}}));
            return true;
        }

        std::cout << "  unknown: " << cmd << " (h for help)\n";
        return true;
    }

    int registerToken(const std::string& configPath)
    {
        json root = loadDeviceList(configPath);
        json& samsung = findSamsungEntry(root);
        if (!samsung.contains("room_id"))
            samsung["room_id"] = "1111111111111111";

        const std::string host = samsung.at("interface").at("host").get<std::string>();
        samsung["interface"].erase("token");

        SamsungTizenTokenClient pairing;
        pairing.configure(samsung);

        std::cout << "Connecting to " << host << "...\n";
        std::cout << "Approve \"WaveHome\" on the TV if prompted.\n" << std::flush;

        const auto result = pairing.requestToken();
        if (!result.ok)
        {
            std::cerr << "register failed: " << result.message << " (code " << result.errorCode << ")\n";
            return 1;
        }

        samsung["interface"]["token"] = result.token;
        saveDeviceList(configPath, root);

        std::cout << "Token saved to " << configPath << '\n';
        std::cout << "  token: " << result.token << '\n';
        if (!result.modelName.empty())
            std::cout << "  model: " << result.modelName << '\n';
        return 0;
    }
}

int main(int argc, const char* argv[])
{
    ArgParser parser("test-samsung-tizen", "Interactive test for Samsung Tizen displays.");
    parser.addArgument("--config", "-c")
        .help("device_list.json path.")
        .defaultValue("bin/device/device_list.json");
    parser.addArgument("--register", "-r")
        .help("Pair with the display, save token to config, and exit.")
        .actionFlag();

    try
    {
        parser.parseArgs(argc, argv);
    }
    catch (const std::exception& ex)
    {
        std::cerr << ex.what() << '\n';
        return 1;
    }

    const std::string configPath = parser.get<std::string>("config");

    try
    {
        if (parser.has("register"))
            return registerToken(configPath);

        json config = findSamsungConfig(loadDeviceList(configPath));
        if (!config.contains("room_id"))
            config["room_id"] = "1111111111111111";

        SamsungTizen tv;
        const int rc = tv.init(config);
        if (rc != 0)
        {
            std::cerr << "tv.init failed: " << rc << " (" << tv.getErrorString(rc) << ")\n";
            if (config["interface"].value("token", "").empty())
                std::cerr << "hint: run with --register first\n";
            return 1;
        }

        std::cout << "ready (" << tv.getInterfaceConfig().host << ")\n";
        printHelp();

        std::string line;
        std::cout << "> " << std::flush;
        while (std::getline(std::cin, line))
        {
            std::istringstream in(line);
            std::string cmd;
            in >> cmd;

            if (!cmd.empty() && !runCommand(tv, cmd, in))
                break;

            std::cout << "> " << std::flush;
        }

        std::cout << "\nbye\n";
        tv.shutdown();
        return 0;
    }
    catch (const std::exception& ex)
    {
        std::cerr << "error: " << ex.what() << '\n';
        return 1;
    }
}
