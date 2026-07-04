#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "core/json.h"
#include "device/platform/philips_wiz_e29.h"
#include "util/arg_parser.h"

using ws::json;
using namespace ws::dev;

namespace
{
    json loadConfig(const std::string& path, const std::string& className)
    {
        std::ifstream in(path);
        if (!in.is_open())
            throw std::runtime_error("failed to open " + path);

        json root;
        in >> root;

        for (const auto& device : root.at("device_list"))
        {
            if (device.at("class").get<std::string>() == className)
                return device;
        }
        throw std::runtime_error(className + " device not found in " + path);
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
        std::cout << "  " << name << " => " << (code == 0 ? "ok" : "fail")
                  << " (code " << code << ")\n";
    }

    void printHelp(const PhilipsWizE29::Capabilities& caps)
    {
        std::cout <<
            "commands:\n"
            "  on | off | toggle                     power control\n"
            "  dim <10-100>                           set brightness\n";
        if (caps.color)
            std::cout <<
            "  color <r> <g> <b>                      set RGB color (0-255)\n";
        if (caps.tunableWhite)
            std::cout <<
            "  temp <kelvin>                          set white color temperature\n";
        std::cout <<
            "  get <state|status|brightness|"
            "capabilities" << (caps.color ? "|color" : "") << (caps.tunableWhite ? "|temperature" : "") << ">\n"
            "  help                                   this help\n"
            "  quit | exit                            shutdown and exit\n";
    }

    void handleGet(PhilipsWizE29& bulb, std::istringstream& args)
    {
        std::string name;
        args >> name;
        if (name.empty())
            name = "status";
        printResult(bulb.query(name, json::object()));
    }

    void handleColor(PhilipsWizE29& bulb, std::istringstream& args)
    {
        int r = -1, g = -1, b = -1;
        args >> r >> g >> b;
        if (r < 0 || g < 0 || b < 0)
        {
            std::cout << "  usage: color <r> <g> <b>   (0-255 each)\n";
            return;
        }
        printInvoke("color", bulb.invoke("color", {{"r", r}, {"g", g}, {"b", b}}));
    }
}

int main(int argc, const char* argv[])
{
    ArgParser parser("test-philips-wiz-e29", "Interactive test for the Philips WiZ E29 bulb.");
    parser.addArgument("--config", "-c")
        .help("device_list.json path.")
        .defaultValue("bin/data/device_list.json");
    parser.addArgument("--class")
        .help("Device class to load.")
        .defaultValue("philips_wiz_e29");
    parser.addArgument("--host")
        .help("Override the bulb host/IP from the config.");

    try
    {
        parser.parseArgs(argc, argv);
    }
    catch (const std::exception& ex)
    {
        std::cerr << ex.what() << '\n';
        return 1;
    }

    try
    {
        json config = loadConfig(parser.get<std::string>("config"), parser.get<std::string>("class"));
        if (!config.contains("room_id"))
            config["room_id"] = "1111111111111111";
        if (parser.has("host"))
            config["interface"]["host"] = parser.get<std::string>("host");

        PhilipsWizE29 bulb;
        const int rc = bulb.init(config);
        if (rc != 0)
        {
            std::cerr << "bulb.init failed: " << rc
                      << " (" << bulb.getErrorString(rc) << ")\n";
            return 1;
        }

        const PhilipsWizE29::Capabilities& caps = bulb.getCapabilities();
        std::cout << "bulb ready (" << bulb.getConfig().host << ", class=" << bulb.getClass()
                  << ", module=" << caps.module << ")\n"
                  << "capabilities: dimming=" << caps.dimming
                  << " color=" << caps.color
                  << " tunableWhite=" << caps.tunableWhite;
        if (caps.tunableWhite)
            std::cout << " temp=" << caps.tempMinK << "-" << caps.tempMaxK << "K";
        std::cout << '\n';
        printHelp(caps);

        std::string line;
        std::cout << ">> " << std::flush;
        while (std::getline(std::cin, line))
        {
            std::istringstream in(line);
            std::string cmd;
            in >> cmd;

            if (cmd.empty()) { /* reprompt */ }
            else if (cmd == "quit" || cmd == "exit") break;
            else if (cmd == "help") printHelp(caps);
            else if (cmd == "on") printInvoke("on", bulb.invoke("on", json::object()));
            else if (cmd == "off") printInvoke("off", bulb.invoke("off", json::object()));
            else if (cmd == "toggle") printInvoke("toggle", bulb.invoke("toggle", json::object()));
            else if (cmd == "dim")
            {
                int value = -1;
                in >> value;
                if (value < 0)
                    std::cout << "  usage: dim <10-100>\n";
                else
                    printInvoke("brightness", bulb.invoke("brightness", {{"value", value}}));
            }
            else if (cmd == "color") handleColor(bulb, in);
            else if (cmd == "temp")
            {
                int kelvin = -1;
                in >> kelvin;
                if (kelvin < 0)
                    std::cout << "  usage: temp <kelvin>\n";
                else
                    printInvoke("temperature", bulb.invoke("temperature", {{"value", kelvin}}));
            }
            else if (cmd == "get") handleGet(bulb, in);
            else std::cout << "  unknown command: " << cmd << " (type 'help')\n";

            std::cout << ">> " << std::flush;
        }

        std::cout << "\nshutting down...\n";
        bulb.shutdown();
        return 0;
    }
    catch (const std::exception& ex)
    {
        std::cerr << "error: " << ex.what() << '\n';
        return 1;
    }
}
