#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>

#include <util/arg_parser.h>
#include <util/spinner.h>

#include "service/llm_client.h"

namespace
{
using ws::json;
using ws::llm::Chat;
using ws::llm::Client;
using ws::llm::Image;
using ws::llm::Message;
using ws::llm::Result;
using ws::llm::Text;

std::string resultToString(Result result)
{
    switch (result)
    {
    case Result::SUCCESS:
        return "SUCCESS";
    case Result::ERROR_NOT_INITIALIZED:
        return "ERROR_NOT_INITIALIZED";
    case Result::ERROR_INVALID_CONFIG:
        return "ERROR_INVALID_CONFIG";
    case Result::ERROR_INVALID_PROTOCOL:
        return "ERROR_INVALID_PROTOCOL";
    case Result::ERROR_NETWORK:
        return "ERROR_NETWORK";
    case Result::ERROR_HTTP:
        return "ERROR_HTTP";
    case Result::ERROR_PARSE:
        return "ERROR_PARSE";
    case Result::ERROR_MODEL:
        return "ERROR_MODEL";
    case Result::ERROR_IO:
        return "ERROR_IO";
    }
    return "UNKNOWN";
}

std::string buildHostUrl(const ArgParser& parser)
{
    const std::string host = parser.get<std::string>("host");
    if (host.find("://") != std::string::npos)
        return host;

    const uint16_t port = static_cast<uint16_t>(parser.get<int>("port"));
    return "http://" + host + ":" + std::to_string(port);
}

json buildModelConfig(const ArgParser& parser)
{
    json model_config;
    model_config["protocol"] = parser.get<std::string>("protocol");
    model_config["host"] = buildHostUrl(parser);
    model_config["model"] = parser.get<std::string>("model");
    model_config["api-key"] = parser.has("api-key") ? parser.get<std::string>("api-key") : "";
    return model_config;
}

Chat::Options buildChatOptions(const ArgParser& parser)
{
    Chat::Options options;
    options.temperature = parser.get<float>("temperature");
    if (parser.has("max-tokens"))
        options.maxTokens = parser.get<int32_t>("max-tokens");
    if (parser.has("top-p"))
        options.topP = parser.get<float>("top-p");
    if (parser.has("frequency-penalty"))
        options.frequencyPenalty = parser.get<float>("frequency-penalty");
    if (parser.has("presence-penalty"))
        options.presencePenalty = parser.get<float>("presence-penalty");
    if (parser.has("seed"))
        options.seed = parser.get<int32_t>("seed");
    if (parser.has("max-context-messages"))
        options.maxContextMessages = static_cast<size_t>(parser.get<int>("max-context-messages"));
    if (parser.has("stop"))
    {
        std::istringstream tokens(parser.get<std::string>("stop"));
        std::string token;
        while (std::getline(tokens, token, ','))
        {
            if (!token.empty())
                options.stop.push_back(token);
        }
    }
    return options;
}

void addUserMessage(Chat& chat, const std::string& text, const std::string& image_path)
{
    auto message = Message::create(Message::Role::User);
    if (!text.empty())
        message->addContent(Text::create(text));
    if (!image_path.empty())
        message->addContent(Image::fromFile(image_path));
    chat.addMessage(std::move(message));
}

class StreamPrinter
{
public:
    explicit StreamPrinter(Spinner& spinner) :
        m_spinner(spinner)
    {
    }

    void onThinking(const std::string& delta)
    {
        if (!m_inThinking)
        {
            m_spinner.stop();
            std::cout << "\nThinking:\n";
            m_inThinking = true;
            m_started = true;
        }
        std::cout << delta << std::flush;
    }

    void onContent(const std::string& delta)
    {
        if (m_inThinking)
        {
            std::cout << "\n\nAnswer:\n";
            m_inThinking = false;
        }
        if (!m_started)
        {
            m_spinner.stop();
            std::cout << "\n";
            m_started = true;
        }
        std::cout << delta << std::flush;
    }

private:
    Spinner& m_spinner;
    bool m_inThinking = false;
    bool m_started = false;
};
}  // namespace

int main(int argc, const char* argv[])
{
    std::cout.setf(std::ios::unitbuf);

    ArgParser parser("test-llm", "Console chat client using ws::llm::Client (OpenAI protocol).");
    parser.addArgument("--host", "-H")
        .help("LLM server host or full base URL.")
        .defaultValue("127.0.0.1");
    parser.addArgument("--port", "-p")
        .help("LLM server port (ignored when --host includes a scheme).")
        .defaultValue("11434");
    parser.addArgument("--protocol")
        .help("Client protocol name (openai-ollama or openai).")
        .defaultValue("openai-ollama");
    parser.addArgument("--model", "-m")
        .help("Model name.")
        .defaultValue("gemma4:12b-mlx");
    parser.addArgument("--api-key", "-k")
        .help("API key (optional).");
    parser.addArgument("--temperature", "-T")
        .help("Sampling temperature.")
        .defaultValue("0.7");
    parser.addArgument("--max-tokens")
        .help("Maximum tokens to generate (0 = server default).");
    parser.addArgument("--top-p")
        .help("Nucleus sampling top_p.");
    parser.addArgument("--frequency-penalty")
        .help("Frequency penalty.");
    parser.addArgument("--presence-penalty")
        .help("Presence penalty.");
    parser.addArgument("--seed")
        .help("Random seed (-1 = omit).");
    parser.addArgument("--stop")
        .help("Stop sequences, comma-separated.");
    parser.addArgument("--max-context-messages")
        .help("Keep only the most recent N messages when sending.");
    parser.addArgument("--system-prompt", "-s")
        .help("System prompt for the conversation.");
    parser.addArgument("--image", "-i")
        .help("Attach an image file to the next user message.");
    parser.addArgument("--no-stream")
        .help("Use non-streaming chat completion.")
        .actionFlag();
    parser.addArgument("--ensure-model")
        .help("Call ensureModelLoaded() before chatting.")
        .actionFlag();

    try
    {
        parser.parseArgs(argc, argv);
    }
    catch (const std::exception& ex)
    {
        std::cerr << ex.what() << "\n";
        return 1;
    }

    const json model_config = buildModelConfig(parser);

    Client client;
    std::string init_error;
    const Result init_result = client.init(model_config, init_error);
    if (init_result != Result::SUCCESS)
    {
        std::cerr << "init failed: " << resultToString(init_result);
        if (!init_error.empty())
            std::cerr << " (" << init_error << ")";
        std::cerr << "\n";
        return 1;
    }

    if (parser.has("ensure-model"))
    {
        Spinner spinner("Ensuring model");
        spinner.start();
        const Result ensure_result = client.ensureModelLoaded();
        spinner.stop();
        if (ensure_result != Result::SUCCESS)
        {
            std::cerr << "ensureModelLoaded failed: " << resultToString(ensure_result) << "\n";
            return 1;
        }
    }

    Chat chat;
    chat.setOptions(buildChatOptions(parser));

    const std::string system_prompt =
        parser.has("system-prompt") ? parser.get<std::string>("system-prompt") : "";
    if (!system_prompt.empty())
        chat.addMessage(Message::create(Message::Role::System, Text::create(system_prompt)));

    std::cout << "Connected to " << client.getHost()
              << " (model: " << client.getModel() << ")\n";
    if (!system_prompt.empty())
        std::cout << "System prompt: " << system_prompt << "\n";
    std::cout << "Type a message and press Enter. Empty line or Ctrl+D to quit.\n\n";

    std::string pending_image =
        parser.has("image") ? parser.get<std::string>("image") : "";

    std::string line;
    while (true)
    {
        std::cout << "> " << std::flush;
        if (!std::getline(std::cin, line))
            break;
        if (line.empty())
            break;

        addUserMessage(chat, line, pending_image);
        pending_image.clear();

        Spinner response_spinner("Responding");
        response_spinner.start();

        StreamPrinter printer(response_spinner);
        Result chat_result = Result::SUCCESS;

        if (parser.has("no-stream"))
        {
            chat_result = client.chat(chat);
            response_spinner.stop();
            if (chat_result == Result::SUCCESS && chat.size() > 0)
            {
                const auto& last = *(chat.end() - 1);
                if (last && last->size() == 1)
                    std::cout << "\n" << (*last->begin())->content() << "\n\n";
            }
        }
        else
        {
            chat_result = client.streamChat(
                chat,
                [&](const std::string& delta) { printer.onContent(delta); },
                [&](const std::string& delta) { printer.onThinking(delta); });
            response_spinner.stop();
            if (chat_result == Result::SUCCESS)
                std::cout << "\n\n";
        }

        if (chat_result != Result::SUCCESS)
        {
            std::cerr << "Chat failed: " << resultToString(chat_result) << "\n";
            if (chat.size() > 0 && (*(chat.end() - 1))->role() == Message::Role::User)
                chat.truncate(chat.size() - 1);
        }
    }

    return 0;
}
