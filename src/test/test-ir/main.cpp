#include "ir/ir_remote.h"
#include "util/arg_parser.h"

#include <chrono>
#include <fstream>
#include <map>
#include <csignal>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>

namespace
{
volatile std::sig_atomic_t g_running = 1;

void onSignal(int)
{
    g_running = 0;
}

const char* resultToString(IRResult result)
{
    switch (result) {
    case IRResult::SUCCESS:
        return "SUCCESS";
    case IRResult::ERROR_INVALID_FORMAT:
        return "ERROR_INVALID_FORMAT";
    case IRResult::ERROR_INVALID_LENGTH:
        return "ERROR_INVALID_LENGTH";
    case IRResult::ERROR_FAILED_TO_OPEN_DEVICE:
        return "ERROR_FAILED_TO_OPEN_DEVICE";
    case IRResult::ERROR_NOT_INITIALIZED:
        return "ERROR_NOT_INITIALIZED";
    case IRResult::ERROR_CMD_ALREADY_EXISTS:
        return "ERROR_CMD_ALREADY_EXISTS";
    case IRResult::ERROR_CMD_NOT_FOUND:
        return "ERROR_CMD_NOT_FOUND";
    case IRResult::ERROR_CMD_EMPTY:
        return "ERROR_CMD_EMPTY";
    case IRResult::ERROR_CMD_DONT_MATCH:
        return "ERROR_CMD_DONT_MATCH";
    case IRResult::ERROR_INCOMPLETE_FRAME:
        return "ERROR_INCOMPLETE_FRAME";
    case IRResult::ERROR_FAILED_TO_RECEIVE:
        return "ERROR_FAILED_TO_RECEIVE";
    case IRResult::ERROR_FAILED_TO_TRANSMIT:
        return "ERROR_FAILED_TO_TRANSMIT";
    }
    return "UNKNOWN";
}

void printPayload(const IRPayload& payload)
{
    switch (payload.type()) {
    case IRPayload::Type::EMPTY:
        std::cout << "type=EMPTY";
        break;
    case IRPayload::Type::CODE_ONLY:
        std::cout << "type=CODE_ONLY code=0x" << std::hex << std::setw(2) << std::setfill('0')
                  << static_cast<int>(payload.code()) << std::dec;
        break;
    case IRPayload::Type::CODE_DATA:
        std::cout << "type=CODE_DATA code=0x" << std::hex << std::setw(2) << std::setfill('0')
                  << static_cast<int>(payload.code()) << " data=0x" << std::setw(2)
                  << static_cast<int>(payload.data()) << std::dec;
        break;
    case IRPayload::Type::REPEAT:
        std::cout << "type=REPEAT";
        break;
    case IRPayload::Type::CUSTOM_28:
        std::cout << "type=CUSTOM_28 raw28=0x" << std::hex << payload.raw28() << std::dec;
        break;
    }
    std::cout << " raw=0x" << std::hex << std::setw(8) << std::setfill('0') << payload.raw()
              << std::dec;
}

void registerDefaultCommands(IRCommandList& list)
{
    const struct {
        const char* name;
        const char* code;
        const char* data;
    } defaults[] = {
        {"power", "0x00", nullptr},
        {"vol_up", "0x40", nullptr},
        {"vol_down", "0x41", nullptr},
        {"mute", "0x09", nullptr},
        {"input_hdmi1", "0x10", "0x01"},
    };

    for (const auto& cmd : defaults) {
        IRResult res = IRResult::SUCCESS;
        if (cmd.data)
            res = list.addCommand(cmd.name, IRPayload(cmd.code, cmd.data));
        else
            res = list.addCommand(cmd.name, IRPayload(cmd.code));

        if (res != IRResult::SUCCESS && res != IRResult::ERROR_CMD_ALREADY_EXISTS) {
            std::cerr << "warning: failed to register '" << cmd.name << "': "
                      << resultToString(res) << "\n";
        }
    }
}

void printCommandList(const IRCommandList& list)
{
    std::map<std::string, IRPayload> sortedCommands(list.begin(), list.end());
    for (const auto& [name, payload] : sortedCommands) {
        std::cout << name << ": ";
        printPayload(payload);
        std::cout << "\n";
    }
}

bool loadCommands(IRCommandList& list, const std::string& filepath)
{
    std::ifstream in(filepath);
    if (!in.is_open()) {
        return false;
    }

    std::string line;
    while (std::getline(in, line)) {
        size_t first = line.find_first_not_of(" \t\r\n");
        if (first == std::string::npos || line[first] == '#') {
            continue;
        }

        std::stringstream ss(line);
        std::string name, code_str, data_str;

        if (!std::getline(ss, name, ',')) continue;
        if (!std::getline(ss, code_str, ',')) continue;
        std::getline(ss, data_str, ',');

        auto trim = [](std::string& s) {
            size_t start = s.find_first_not_of(" \t\r\n");
            if (start == std::string::npos) {
                s.clear();
                return;
            }
            size_t end = s.find_last_not_of(" \t\r\n");
            s = s.substr(start, end - start + 1);
        };
        trim(name);
        trim(code_str);
        trim(data_str);

        if (name.empty() || code_str.empty()) continue;

        IRPayload payload;
        if (code_str.starts_with("raw28:")) {
            uint32_t val = std::stoul(code_str.substr(6), nullptr, 16);
            payload = IRPayload::fromRaw28(val);
        } else if (!data_str.empty()) {
            payload = IRPayload(code_str, data_str);
        } else {
            payload = IRPayload(code_str);
        }

        if (payload.valid()) {
            list.removeCommand(name);
            list.addCommand(name, payload);
        }
    }
    return true;
}

bool saveCommands(const IRCommandList& list, const std::string& filepath)
{
    std::ofstream out(filepath);
    if (!out.is_open()) {
        return false;
    }

    std::map<std::string, IRPayload> sortedCommands(list.begin(), list.end());

    out << "# Format: name,code[,data]\n";
    for (const auto& [name, payload] : sortedCommands) {
        if (payload.type() == IRPayload::Type::CODE_ONLY) {
            out << name << ",0x" << std::hex << std::setw(2) << std::setfill('0')
                << static_cast<int>(payload.code()) << "\n";
        } else if (payload.type() == IRPayload::Type::CODE_DATA) {
            out << name << ",0x" << std::hex << std::setw(2) << std::setfill('0')
                << static_cast<int>(payload.code()) << ",0x" << std::setw(2) << std::setfill('0')
                << static_cast<int>(payload.data()) << "\n";
        } else if (payload.type() == IRPayload::Type::CUSTOM_28) {
            out << name << ",raw28:0x" << std::hex << payload.raw28() << "\n";
        }
    }
    return true;
}

int runLearn(ArgParser& parser, IRReceiver& rx, IRCommandList& list, const std::string& filepath)
{
    if (parser.has("learn")) {
        const std::string name = parser.get<std::string>("learn");
        if (name.empty()) {
            std::cerr << "error: command name cannot be empty\n";
            return 1;
        }

        std::cout << "Learning mode: Press the remote button for '" << name << "' now (Timeout: 10s)...\n";

        constexpr int kMaxAttempts = 100;
        for (int i = 0; i < kMaxAttempts; ++i) {
            if (rx.hasData()) {
                IRPayload payload;
                const IRResult res = rx.recv(payload);

                if (res == IRResult::SUCCESS) {
                    if (payload.type() == IRPayload::Type::REPEAT) {
                        std::cout << "Received repeat code. Please try again...\n";
                        std::this_thread::sleep_for(std::chrono::milliseconds(200));
                        continue;
                    }

                    list.removeCommand(name);

                    IRResult addRes = list.addCommand(name, payload);
                    if (addRes != IRResult::SUCCESS) {
                        std::cerr << "Failed to register learned command: " << resultToString(addRes) << "\n";
                        return 1;
                    }

                    if (!saveCommands(list, filepath)) {
                        std::cerr << "warning: failed to save learned command to " << filepath << "\n";
                    }

                    std::cout << "Successfully learned and saved command '" << name << "': ";
                    printPayload(payload);
                    std::cout << "\n";
                    return 0;
                }

                if (res != IRResult::ERROR_INCOMPLETE_FRAME) {
                    std::cerr << "Failed to receive: " << resultToString(res) << "\n";
                    return 1;
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        std::cerr << "Learning timed out.\n";
        return 1;
    }

    return -1;
}

int runTransmit(ArgParser& parser, IRTransmitter& tx)
{
    if (parser.has("send")) {
        const std::string cmd = parser.get<std::string>("send");
        IRResult res = IRResult::SUCCESS;

        if (parser.has("data")) {
            const std::string data = parser.get<std::string>("data");
            res = tx.send(cmd, data);
        } else {
            res = tx.send(cmd);
        }

        if (res != IRResult::SUCCESS) {
            std::cerr << "send failed: " << resultToString(res) << "\n";
            return 1;
        }

        std::cout << "sent command '" << cmd << "'\n";
        return 0;
    }

    if (parser.has("raw-code")) {
        const std::string code = parser.get<std::string>("raw-code");
        IRPayload payload(code);

        if (parser.has("raw-data")) {
            const std::string data = parser.get<std::string>("raw-data");
            payload = IRPayload(code, data);
        }

        if (!payload.valid()) {
            std::cerr << "invalid raw payload format\n";
            return 1;
        }

        const IRResult res = tx.send(payload);
        if (res != IRResult::SUCCESS) {
            std::cerr << "send failed: " << resultToString(res) << "\n";
            return 1;
        }

        std::cout << "sent raw payload: ";
        printPayload(payload);
        std::cout << "\n";
        return 0;
    }

    return -1;
}

int runRemote(ArgParser& parser, IRTransmitter& tx, const IRCommandList& list)
{
    std::map<std::string, IRPayload> sortedCommands(list.begin(), list.end());
    std::vector<std::pair<std::string, IRPayload>> commands(sortedCommands.begin(), sortedCommands.end());

    if (commands.empty()) {
        std::cerr << "error: no commands registered. Please run learning mode first or check commands.txt\n";
        return 1;
    }

    std::signal(SIGINT, onSignal);

    while (g_running) {
        std::cout << "\n=== Virtual IR Remote ===\n";
        for (size_t i = 0; i < commands.size(); ++i) {
            std::cout << (i + 1) << ". " << commands[i].first << "\n";
        }
        std::cout << "0. Exit\n";
        std::cout << "Select command (0-" << commands.size() << "): ";

        std::string input;
        if (!std::getline(std::cin, input)) {
            break;
        }

        size_t start = input.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) {
            continue;
        }
        size_t end = input.find_last_not_of(" \t\r\n");
        input = input.substr(start, end - start + 1);

        if (input == "0") {
            std::cout << "Exiting remote control mode.\n";
            break;
        }

        try {
            size_t sel = std::stoul(input);
            if (sel > 0 && sel <= commands.size()) {
                const auto& [name, payload] = commands[sel - 1];
                std::cout << "Sending command '" << name << "'...\n";
                IRResult res = tx.send(payload);
                if (res == IRResult::SUCCESS) {
                    std::cout << "Sent successfully.\n";
                } else {
                    std::cerr << "Failed to send: " << resultToString(res) << "\n";
                }
            } else {
                std::cout << "Invalid selection. Please choose between 0 and " << commands.size() << ".\n";
            }
        } catch (const std::exception&) {
            std::cout << "Invalid input. Please enter a number.\n";
        }
    }

    return 0;
}

int runReceive(ArgParser& parser, IRReceiver& rx)
{
    if (parser.has("recv")) {
        constexpr int kMaxAttempts = 50;
        for (int i = 0; i < kMaxAttempts; ++i) {
            if (rx.hasData()) {
                IRPayload payload;
                std::string cmdName;
                const IRResult res = rx.recv(payload, cmdName);

                if (res == IRResult::SUCCESS) {
                    std::cout << "received command '" << cmdName << "': ";
                    printPayload(payload);
                    std::cout << "\n";
                    return 0;
                }

                if (res == IRResult::ERROR_CMD_NOT_FOUND) {
                    std::cout << "received unknown payload: ";
                    printPayload(payload);
                    std::cout << "\n";
                    return 0;
                }

                std::cerr << "recv failed: " << resultToString(res) << "\n";
                return 1;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        std::cerr << "recv timed out: " << resultToString(IRResult::ERROR_INCOMPLETE_FRAME)
                  << "\n";
        return 1;
    }

    if (parser.has("listen")) {
        std::signal(SIGINT, onSignal);
        std::cout << "listening for IR signals (Ctrl+C to stop)...\n";

        while (g_running) {
            if (!rx.hasData()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }

            IRPayload payload;
            std::string cmdName;
            const IRResult res = rx.recv(payload, cmdName);

            if (res == IRResult::SUCCESS) {
                std::cout << "command='" << cmdName << "' ";
                printPayload(payload);
                std::cout << "\n";
            } else if (res == IRResult::ERROR_CMD_NOT_FOUND) {
                std::cout << "unknown ";
                printPayload(payload);
                std::cout << "\n";
            } else if (res != IRResult::ERROR_INCOMPLETE_FRAME) {
                std::cerr << "recv error: " << resultToString(res) << "\n";
            }
        }

        std::cout << "\n";
        return 0;
    }

    return -1;
}

int runRawDump(ArgParser& parser)
{
    const std::string rxDev = parser.get<std::string>("rx-dev");
    const int fd = open(rxDev.c_str(), O_RDONLY);
    if (fd < 0) {
        std::cerr << "Failed to open receiver device: " << rxDev << " (" << strerror(errno) << ")\n";
        return 1;
    }

    std::signal(SIGINT, onSignal);
    std::cout << "Dumping raw LIRC packets from " << rxDev << ". Press Ctrl+C to exit.\n";

    uint32_t packet;
    while (g_running) {
        ssize_t bytesRead = read(fd, &packet, sizeof(packet));
        if (bytesRead == sizeof(packet)) {
            uint32_t mode = packet & 0xFF000000u;
            uint32_t val = packet & 0x00FFFFFFu;
            if (mode == 0x01000000u) {
                std::cout << "pulse " << val << "\n";
            } else if (mode == 0x00000000u) {
                std::cout << "space " << val << "\n";
            } else if (mode == 0x03000000u) {
                std::cout << "timeout " << val << "\n";
            } else {
                std::cout << "other mode=0x" << std::hex << mode << std::dec << " val=" << val << "\n";
            }
        } else if (bytesRead < 0) {
            if (errno == EINTR) continue;
            std::cerr << "read error: " << strerror(errno) << "\n";
            break;
        }
    }

    close(fd);
    return 0;
}
}  // namespace

int main(int argc, const char* argv[])
{
    ArgParser parser("test-ir", "IR remote send/receive test for Raspberry Pi LIRC devices.");
    parser.addArgument("--tx-dev", "-t")
        .help("Transmit device path.")
        .defaultValue("/dev/lirc0");
    parser.addArgument("--rx-dev", "-r")
        .help("Receive device path.")
        .defaultValue("/dev/lirc1");
    parser.addArgument("--send", "-s").help("Send a registered command by name.");
    parser.addArgument("--data", "-d").help("Optional data byte for send (hex or binary string).");
    parser.addArgument("--raw-code").help("Send raw code without command list lookup.");
    parser.addArgument("--raw-data").help("Optional raw data byte with --raw-code.");
    parser.addArgument("--recv").help("Receive one IR frame and print result.").actionFlag();
    parser.addArgument("--listen", "-l")
        .help("Continuously receive IR frames.")
        .actionFlag();
    parser.addArgument("--learn").help("Learn a new IR command and save it to file by name.");
    parser.addArgument("--raw").help("Dump raw LIRC packets from the receiver device.").actionFlag();
    parser.addArgument("--remote").help("Enter interactive remote control mode.").actionFlag();
    parser.addArgument("--list").help("List registered commands and exit.").actionFlag();

    try {
        parser.parseArgs(argc, argv);
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << "\n";
        return 1;
    }

    if (parser.has("raw")) {
        return runRawDump(parser);
    }

    auto cmdList = std::make_shared<IRCommandList>();
    const std::string filepath = "/home/radai/wave-home2/src/common/ir/commands.txt";

    if (!loadCommands(*cmdList, filepath)) {
        registerDefaultCommands(*cmdList);
        saveCommands(*cmdList, filepath);
    }

    if (parser.has("list")) {
        printCommandList(*cmdList);
        return 0;
    }

    const bool wantsTx = parser.has("send") || parser.has("raw-code") || parser.has("remote");
    const bool wantsRx = parser.has("recv") || parser.has("listen") || parser.has("learn");

    if (!wantsTx && !wantsRx) {
        std::cerr << "error: specify --send, --raw-code, --recv, --listen, --learn, --remote, or --list\n";
        parser.printHelp();
        return 1;
    }

    IRTransmitter tx(cmdList);
    IRReceiver rx(cmdList);

    if (wantsTx) {
        const IRResult res = tx.init(parser.get<std::string>("tx-dev"));
        if (res != IRResult::SUCCESS) {
            std::cerr << "tx init failed: " << resultToString(res) << "\n";
            return 1;
        }
    }

    if (wantsRx) {
        const IRResult res = rx.init(parser.get<std::string>("rx-dev"));
        if (res != IRResult::SUCCESS) {
            std::cerr << "rx init failed: " << resultToString(res) << "\n";
            return 1;
        }
    }

    if (wantsTx) {
        if (parser.has("remote")) {
            const int code = runRemote(parser, tx, *cmdList);
            if (code >= 0)
                return code;
        } else {
            const int code = runTransmit(parser, tx);
            if (code >= 0)
                return code;
        }
    }

    if (wantsRx) {
        if (parser.has("learn")) {
            const int code = runLearn(parser, rx, *cmdList, filepath);
            if (code >= 0)
                return code;
        } else {
            const int code = runReceive(parser, rx);
            if (code >= 0)
                return code;
        }
    }

    return 0;
}
