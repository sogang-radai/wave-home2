#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <llm/lm_studio_client.h>
#include <util/spinner.h>
#include <util/arg_parser.h>

namespace
{
constexpr const char* kTokenRelativePath = "../../test/test-llm/token.txt";

std::string getExecutableDir()
{
    std::error_code ec;
    const auto exePath = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec)
        return std::filesystem::current_path().string();
    return exePath.parent_path().string();
}

std::string readTokenFile()
{
    const std::filesystem::path tokenPath =
        std::filesystem::path(getExecutableDir()) / kTokenRelativePath;

    std::ifstream file(tokenPath);
    if (!file.is_open())
        return "";

    std::string token;
    std::getline(file, token);
    return token;
}

std::string resolveToken(const ArgParser& parser)
{
    if (parser.has("token"))
        return parser.get<std::string>("token");

    const std::string fileToken = readTokenFile();
    if (!fileToken.empty())
        return fileToken;

    return "";
}
}  // namespace

int main(int argc, const char* argv[])
{
    ArgParser parser("test-llm",
                     "Console chat client for LM Studio (streaming).");
    parser.addArgument("--host", "-H")
        .help("LM Studio server host.")
        .defaultValue("192.168.0.47");
    parser.addArgument("--port", "-p")
        .help("LM Studio server port.")
        .defaultValue("1620");
    parser.addArgument("--token", "-t")
        .help("API token (overrides token.txt).");
    parser.addArgument("--temperature", "-T")
        .help("Sampling temperature [0, 1].")
        .defaultValue("0.7");
    parser.addArgument("--model", "-m")
        .help("Model key to use.")
        .defaultValue("google/gemma-4-e4b");
    parser.addArgument("--system-prompt", "-s")
        .help("System prompt for the conversation.");
    parser.addArgument("--list-models", "-l")
        .help("List available models and exit.")
        .actionFlag();

    try {
        parser.parseArgs(argc, argv);
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << "\n";
        return 1;
    }

    const std::string host = parser.get<std::string>("host");
    const uint16_t port = static_cast<uint16_t>(parser.get<int>("port"));
    const float temperature = parser.get<float>("temperature");
    const std::string model = parser.get<std::string>("model");
    const std::string token = resolveToken(parser);
    const std::string systemPrompt =
        parser.has("system-prompt") ? parser.get<std::string>("system-prompt") : "";

    LmStudioClient client(host, port, token);

    if (parser.has("list-models")) {
        client.listModels();
        return 0;
    }

    Spinner spinner("Preparing");
    spinner.start();

    try {
        if (!client.isModelLoaded(model)) {
            spinner.stop();
            std::cout << "Model '" << model << "' is not loaded. Loading...\n";
            spinner.start();
            client.loadModel(model);
        }
    } catch (const std::exception& ex) {
        spinner.stop();
        std::cerr << "Model setup failed: " << ex.what() << "\n";
        return 1;
    }

    spinner.stop();
    std::cout << "Connected to " << host << ":" << port
              << " (model: " << model << ")\n";
    if (!systemPrompt.empty())
        std::cout << "System prompt: " << systemPrompt << "\n";
    std::cout << "Type a message and press Enter. Empty line or Ctrl+D to quit.\n\n";

    std::string line;
    while (true) {
        std::cout << "> " << std::flush;
        if (!std::getline(std::cin, line))
            break;
        if (line.empty())
            break;

        Spinner responseSpinner("Responding");
        responseSpinner.start();

        bool firstChunk = true;
        try {
            client.streamChat(
                model,
                line,
                temperature,
                systemPrompt,
                [&](const std::string& delta) {
                    if (firstChunk) {
                        responseSpinner.stop();
                        std::cout << "\n";
                        firstChunk = false;
                    }
                    std::cout << delta << std::flush;
                },
                [&](const std::string& status) {
                    responseSpinner.setLabel(status);
                });
        } catch (const std::exception& ex) {
            responseSpinner.stop();
            std::cerr << "\nError: " << ex.what() << "\n";
            continue;
        }

        responseSpinner.stop();
        std::cout << "\n\n";
    }

    return 0;
}
