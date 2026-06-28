#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

#include <util/arg_parser.h>
#include "tcp_server.hpp"

namespace
{
    r4sn::SyncMode parseSyncMode(const std::string& value)
    {
        if (value == "r4sn")
            return r4sn::SyncMode::R4sn;
        if (value != "none")
            throw std::runtime_error("unsupported --sync value: " + value);
        return r4sn::SyncMode::None;
    }
}  // namespace

int main(int argc, char* argv[])
{
    ArgParser parser("iq-server", "Range FFT cube IQ extraction server for r4sn radar.");
    parser.addArgument("--port", "-p").help("IQ server TCP port.").defaultValue("29171");
    parser.addArgument("--sync", "-s")
        .help("Frame sync mode: none or r4sn.")
        .defaultValue("none");
    parser.addArgument("--sync-port")
        .help("Point cloud TCP port used for r4sn sync.")
        .defaultValue("29172");

    try
    {
        parser.parseArgs(argc, const_cast<const char**>(argv));
    }
    catch (const std::exception& ex)
    {
        std::cerr << ex.what() << '\n';
        return 1;
    }

    try
    {
        const uint16_t port = static_cast<uint16_t>(parser.get<int>("port"));
        const uint16_t sync_port = static_cast<uint16_t>(parser.get<int>("sync-port"));
        const auto sync_mode = parseSyncMode(parser.get<std::string>("sync"));

        r4sn::TcpServer server(port, sync_mode, sync_port);
        server.run();
    }
    catch (const std::exception& ex)
    {
        std::cerr << "iq-server error: " << ex.what() << '\n';
        return 1;
    }

    return 0;
}
