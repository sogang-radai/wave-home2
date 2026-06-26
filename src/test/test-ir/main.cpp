#include "ir/ir_remote.h"
#include "util/arg_parser.h"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>

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

        if (res != IRResult::SUCCESS) {
            std::cerr << "warning: failed to register '" << cmd.name << "': "
                      << resultToString(res) << "\n";
        }
    }
}

void printCommandList(const IRCommandList& list)
{
    for (const auto& [name, payload] : list) {
        std::cout << name << ": ";
        printPayload(payload);
        std::cout << "\n";
    }
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
    parser.addArgument("--list").help("List registered commands and exit.").actionFlag();

    try {
        parser.parseArgs(argc, argv);
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << "\n";
        return 1;
    }

    auto cmdList = std::make_shared<IRCommandList>();
    registerDefaultCommands(*cmdList);

    if (parser.has("list")) {
        printCommandList(*cmdList);
        return 0;
    }

    const bool wantsTx = parser.has("send") || parser.has("raw-code");
    const bool wantsRx = parser.has("recv") || parser.has("listen");

    if (!wantsTx && !wantsRx) {
        std::cerr << "error: specify --send, --raw-code, --recv, --listen, or --list\n";
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
        const int code = runTransmit(parser, tx);
        if (code >= 0)
            return code;
    }

    if (wantsRx) {
        const int code = runReceive(parser, rx);
        if (code >= 0)
            return code;
    }

    return 0;
}
