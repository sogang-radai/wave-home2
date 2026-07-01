#include "device/protocol/ir_remote.h"
#include "util/arg_parser.h"

#include <chrono>
#include <csignal>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <optional>
#include <thread>
#include <unistd.h>

using ir::CommandList;
using ir::Payload;
using ir::Receiver;
using ir::Result;
using ir::Transmitter;

namespace
{
    constexpr uint32_t kLircPulseBit = 0x01000000u;
    constexpr uint32_t kLircMode2Mask = 0xFF000000u;
    constexpr uint32_t kLircValueMask = 0x00FFFFFFu;
    constexpr uint32_t kLircMode2Space = 0x00000000u;
    constexpr uint32_t kLircMode2Timeout = 0x03000000u;

    volatile std::sig_atomic_t g_running = 1;

    void onSignal(int)
    {
        g_running = 0;
    }

    void printPayload(const Payload& payload)
    {
        std::cout << "kind=" << Payload::kindToString(payload.kind())
                  << " protocol=" << Payload::protocolToString(payload.protocol());

        switch (payload.kind()) {
        case Payload::Kind::Empty:
            break;
        case Payload::Kind::NecCodeOnly:
            std::cout << " code=0x" << std::hex << std::setw(2) << std::setfill('0')
                      << static_cast<int>(payload.code()) << std::dec;
            break;
        case Payload::Kind::NecCodeData:
            std::cout << " code=0x" << std::hex << std::setw(2) << std::setfill('0')
                      << static_cast<int>(payload.code()) << " data=0x" << std::setw(2)
                      << static_cast<int>(payload.data()) << std::dec;
            break;
        case Payload::Kind::Repeat:
            break;
        case Payload::Kind::LgAc28:
            std::cout << " raw28=0x" << std::hex << payload.raw28() << std::dec;
            break;
        default:
            if (Payload::isRawKind(payload.kind())) {
                std::cout << " bits=" << static_cast<int>(payload.bitCount())
                          << " raw=0x" << std::hex << payload.rawBits() << std::dec;
            }
            break;
        }

        if (payload.isNec())
            std::cout << " necWire32=0x" << std::hex << std::setw(8) << std::setfill('0')
                      << payload.necWire32() << std::dec;
    }

    void registerDefaultCommands(CommandList& list)
    {
        const struct {
            const char* name;
            uint8_t code;
            std::optional<uint8_t> data;
        } defaults[] = {
            {"power", 0x00, std::nullopt},
            {"vol_up", 0x00, 0x40},
            {"vol_down", 0x00, 0x41},
            {"mute", 0x00, 0x09},
            {"input_hdmi1", 0x01, 0x10},
        };

        for (const auto& cmd : defaults) {
            Result res = Result::SUCCESS;
            if (cmd.data)
                res = list.addCommand(cmd.name, Payload(cmd.code, *cmd.data));
            else
                res = list.addCommand(cmd.name, Payload(cmd.code));

            if (res != Result::SUCCESS && res != Result::ERROR_CMD_ALREADY_EXISTS) {
                std::cerr << "warning: failed to register '" << cmd.name << "': "
                          << ir::to_string(res) << "\n";
            }
        }
    }

    void printCommandList(const CommandList& list)
    {
        for (const auto& [name, payload] : list.getSortedCommands()) {
            std::cout << name << ": ";
            printPayload(payload);
            std::cout << "\n";
        }
    }

    int runLearn(ArgParser& parser, Receiver& rx, CommandList& list, const std::string& filepath)
    {
        if (!parser.has("learn"))
            return -1;

        const std::string name = parser.get<std::string>("learn");
        if (name.empty()) {
            std::cerr << "error: command name cannot be empty\n";
            return 1;
        }

        const int timeoutSec = std::stoi(parser.get<std::string>("timeout"));
        std::cout << "Learning mode: Press the remote button for '" << name
                  << "' now (Timeout: " << timeoutSec << "s)...\n";

        const int maxAttempts = timeoutSec * 10;
        for (int i = 0; i < maxAttempts; ++i) {
            if (rx.hasData()) {
                Payload payload;
                const Result res = rx.recv(payload);

                if (res == Result::SUCCESS) {
                    if (payload.kind() == Payload::Kind::Repeat) {
                        std::cout << "Received repeat code. Please try again...\n";
                        std::this_thread::sleep_for(std::chrono::milliseconds(200));
                        continue;
                    }

                    const Result addRes = list.addCommand(name, payload);
                    if (addRes != Result::SUCCESS) {
                        std::cerr << "Failed to register learned command: "
                                  << ir::to_string(addRes) << "\n";
                        return 1;
                    }

                    if (!list.saveToFile(filepath))
                        std::cerr << "warning: failed to save learned command to " << filepath << "\n";

                    std::cout << "Successfully learned and saved command '" << name << "': ";
                    printPayload(payload);
                    std::cout << "\n";
                    return 0;
                }

                if (res != Result::ERROR_INCOMPLETE_FRAME) {
                    std::cerr << "Failed to receive: " << ir::to_string(res) << "\n";
                    return 1;
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        std::cerr << "Learning timed out.\n";
        return 1;
    }

    int runTransmit(ArgParser& parser, Transmitter& tx)
    {
        if (parser.has("send")) {
            const std::string cmd = parser.get<std::string>("send");
            Result res = Result::SUCCESS;

            if (parser.has("data"))
                res = tx.send(cmd, parser.get<std::string>("data"));
            else
                res = tx.send(cmd);

            if (res != Result::SUCCESS) {
                std::cerr << "send failed: " << ir::to_string(res) << "\n";
                return 1;
            }

            std::cout << "sent command '" << cmd << "'\n";
            return 0;
        }

        if (parser.has("raw-code")) {
            const std::string code = parser.get<std::string>("raw-code");
            Payload payload(code);

            if (parser.has("raw-data"))
                payload = Payload(code, parser.get<std::string>("raw-data"));

            if (!payload.valid()) {
                std::cerr << "invalid raw payload format\n";
                return 1;
            }

            const Result res = tx.send(payload);
            if (res != Result::SUCCESS) {
                std::cerr << "send failed: " << ir::to_string(res) << "\n";
                return 1;
            }

            std::cout << "sent raw payload: ";
            printPayload(payload);
            std::cout << "\n";
            return 0;
        }

        return -1;
    }

    int runRemote(ArgParser& parser, Transmitter& tx, const CommandList& list)
    {
        auto commands = list.getSortedCommands();

        if (commands.empty()) {
            std::cerr << "error: no commands registered. Check ir_command.json\n";
            return 1;
        }

        std::signal(SIGINT, onSignal);

        while (g_running) {
            std::cout << "\n=== Virtual IR Remote ===\n";
            for (size_t i = 0; i < commands.size(); ++i)
                std::cout << (i + 1) << ". " << commands[i].first << "\n";
            std::cout << "0. Exit\n";
            std::cout << "Select command (0-" << commands.size() << "): ";

            std::string input;
            if (!std::getline(std::cin, input))
                break;

            const size_t start = input.find_first_not_of(" \t\r\n");
            if (start == std::string::npos)
                continue;
            const size_t end = input.find_last_not_of(" \t\r\n");
            input = input.substr(start, end - start + 1);

            if (input == "0") {
                std::cout << "Exiting remote control mode.\n";
                break;
            }

            try {
                const size_t sel = std::stoul(input);
                if (sel > 0 && sel <= commands.size()) {
                    const auto& [name, payload] = commands[sel - 1];
                    std::cout << "Sending command '" << name << "'...\n";
                    const Result res = tx.send(payload);
                    if (res == Result::SUCCESS)
                        std::cout << "Sent successfully.\n";
                    else
                        std::cerr << "Failed to send: " << ir::to_string(res) << "\n";
                } else {
                    std::cout << "Invalid selection. Please choose between 0 and "
                              << commands.size() << ".\n";
                }
            } catch (const std::exception&) {
                std::cout << "Invalid input. Please enter a number.\n";
            }
        }

        return 0;
    }

    int runReceive(ArgParser& parser, Receiver& rx)
    {
        if (parser.has("recv")) {
            const int maxAttempts = std::stoi(parser.get<std::string>("attempts"));
            for (int i = 0; i < maxAttempts; ++i) {
                if (rx.hasData()) {
                    Payload payload;
                    std::string cmdName;
                    const Result res = rx.recv(payload, cmdName);

                    if (res == Result::SUCCESS) {
                        std::cout << "received command '" << cmdName << "': ";
                        printPayload(payload);
                        std::cout << "\n";
                        return 0;
                    }

                    if (res == Result::ERROR_CMD_NOT_FOUND) {
                        std::cout << "received unknown payload: ";
                        printPayload(payload);
                        std::cout << "\n";
                        return 0;
                    }

                    std::cerr << "recv failed: " << ir::to_string(res) << "\n";
                    return 1;
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

            std::cerr << "recv timed out: " << ir::to_string(Result::ERROR_INCOMPLETE_FRAME) << "\n";
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

                Payload payload;
                std::string cmdName;
                const Result res = rx.recv(payload, cmdName);

                if (res == Result::SUCCESS) {
                    std::cout << "command='" << cmdName << "' ";
                    printPayload(payload);
                    std::cout << "\n";
                } else if (res == Result::ERROR_CMD_NOT_FOUND) {
                    std::cout << "unknown ";
                    printPayload(payload);
                    std::cout << "\n";
                } else if (res != Result::ERROR_INCOMPLETE_FRAME) {
                    std::cerr << "recv error: " << ir::to_string(res) << "\n";
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
            std::cerr << "Failed to open receiver device: " << rxDev
                      << " (" << strerror(errno) << ")\n";
            return 1;
        }

        std::signal(SIGINT, onSignal);
        std::cout << "Dumping raw LIRC packets from " << rxDev << ". Press Ctrl+C to exit.\n";

        uint32_t packet;
        while (g_running) {
            const ssize_t bytesRead = read(fd, &packet, sizeof(packet));
            if (bytesRead == sizeof(packet)) {
                const uint32_t mode = packet & kLircMode2Mask;
                const uint32_t val = packet & kLircValueMask;
                if (mode == kLircPulseBit)
                    std::cout << "pulse " << val << "\n";
                else if (mode == kLircMode2Space)
                    std::cout << "space " << val << "\n";
                else if (mode == kLircMode2Timeout)
                    std::cout << "timeout " << val << "\n";
                else
                    std::cout << "other mode=0x" << std::hex << mode << std::dec << " val=" << val << "\n";
            } else if (bytesRead < 0) {
                if (errno == EINTR)
                    continue;
                std::cerr << "read error: " << strerror(errno) << "\n";
                break;
            }
        }

        close(fd);
        return 0;
    }
}

int main(int argc, const char* argv[])
{
    ArgParser parser("test-ir", "IR remote send/receive test for Raspberry Pi LIRC devices.");
    parser.addArgument("--tx-dev", "-t")
        .help("Transmit device path.")
        .defaultValue("/dev/lirc0");
    parser.addArgument("--rx-dev", "-r")
        .help("Receive device path.")
        .defaultValue("/dev/lirc1");
    parser.addArgument("--commands", "-c")
        .help("IR command list JSON path.")
        .defaultValue("src/resource/ir_command.json");
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
    parser.addArgument("--timeout")
        .help("Timeout in seconds for learning mode (default: 10).")
        .defaultValue("10");
    parser.addArgument("--attempts")
        .help("Maximum attempts (100ms intervals) to wait in single receive mode (default: 50).")
        .defaultValue("50");

    try {
        parser.parseArgs(argc, argv);
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << "\n";
        return 1;
    }

    if (parser.has("raw"))
        return runRawDump(parser);

    auto cmdList = std::make_shared<CommandList>();
    const std::string filepath = parser.get<std::string>("commands");

    if (!cmdList->loadFromFile(filepath)) {
        registerDefaultCommands(*cmdList);
        cmdList->saveToFile(filepath);
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

    Transmitter tx(cmdList);
    Receiver rx(cmdList);

    if (wantsTx) {
        const Result res = tx.init(parser.get<std::string>("tx-dev"));
        if (res != Result::SUCCESS) {
            std::cerr << "tx init failed: " << ir::to_string(res) << "\n";
            return 1;
        }
    }

    if (wantsRx) {
        const Result res = rx.init(parser.get<std::string>("rx-dev"));
        if (res != Result::SUCCESS) {
            std::cerr << "rx init failed: " << ir::to_string(res) << "\n";
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
