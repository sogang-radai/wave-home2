#include <fstream>
#include <iostream>
#include <string>

#include "core/json.h"
#include "device/platform/tuya_ep2h.h"

using ws::json;
using namespace ws::dev;

static json loadDeviceConfig(const std::string& path)
{
    std::ifstream in(path);
    if (!in.is_open())
        throw std::runtime_error("failed to open " + path);

    json root;
    in >> root;

    for (const auto& device : root.at("device_list"))
    {
        if (device.at("class").get<std::string>() == "tuya_ep2h")
            return device;
    }

    throw std::runtime_error("tuya_ep2h device not found in " + path);
}

static void printQuery(TuyaEP2H& plug, const std::string& name)
{
    const json result = plug.query(name, json::object());
    std::cout << "query " << name << " => ";
    if (result.contains("code"))
    {
        const int code = result["code"].get<int>();
        std::cout << "code " << code;
        if (result.contains("message"))
            std::cout << " (" << result["message"].get<std::string>() << ")";
        else
            std::cout << " (" << plug.getErrorString(code) << ")";
    }
    else
        std::cout << result.dump();
    std::cout << '\n';
}

int main(int argc, char* argv[])
{
    const std::string configPath = argc > 1 ? argv[1] : "bin/data/device_list.json";

    try
    {
        const json config = loadDeviceConfig(configPath);
        TuyaEP2H plug;

        const int initCode = plug.init(config);
        std::cout << "init => " << initCode << " (" << plug.getErrorString(initCode) << ")\n";
        if (initCode != 0)
            return 1;

        printQuery(plug, "status");
        printQuery(plug, "switch");
        printQuery(plug, "voltage");
        printQuery(plug, "current");
        printQuery(plug, "power");
        printQuery(plug, "energy");

        if (argc > 2)
        {
            const std::string action = argv[2];
            const int invokeCode = plug.invoke(action, json::object());
            std::cout << "invoke " << action << " => " << invokeCode
                      << " (" << plug.getErrorString(invokeCode) << ")\n";
            printQuery(plug, "switch");
        }

        plug.shutdown();
        return 0;
    }
    catch (const std::exception& ex)
    {
        std::cerr << "error: " << ex.what() << '\n';
        return 1;
    }
}
