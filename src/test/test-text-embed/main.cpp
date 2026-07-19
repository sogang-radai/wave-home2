#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include <util/arg_parser.h>
#include <util/spinner.h>

#include "service/agent/embedding_client.h"

namespace
{
using ws::json;
using ws::embedding::EmbeddingClient;
using ws::embedding::EmbeddingVector;
using ws::embedding::Result;

constexpr const char* kReset = "\x1b[0m";
constexpr const char* kBold = "\x1b[1m";
constexpr const char* kDim = "\x1b[2m";

enum class Metric
{
    Dot,
    Cosine,
};

struct Entry
{
    std::string text;
    EmbeddingVector vector;
};

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

const std::vector<std::string>& sampleSentences()
{
    static const std::vector<std::string> samples = {
        // pets / animals
        "I love playing with my dog.",
        "Cats are independent and curious animals.",
        "The puppy wagged its tail happily.",
        // weather
        "The weather is clear and sunny today.",
        "It rained heavily all through the night.",
        "A cold wind blew across the mountains.",
        // technology
        "Machine learning models require large datasets.",
        "The new smartphone has a much faster processor.",
        "Cloud computing has transformed the software industry.",
        // food
        "This restaurant serves delicious Italian pasta.",
        "I baked a chocolate cake for the party.",
        "Fresh vegetables are essential for a healthy diet.",
        // sports
        "The striker scored a goal in the final minute.",
        "She trained hard for the upcoming marathon.",
        // emotions / misc
        "He felt overjoyed when he heard the good news.",
        "Reading books expands your imagination.",
        "The stock market dropped sharply this morning.",
        "The ancient castle stood on top of the hill.",
    };
    return samples;
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
    json config;
    config["protocol"] = parser.get<std::string>("protocol");
    config["host"] = buildHostUrl(parser);
    config["model"] = parser.get<std::string>("model");
    config["api-key"] = parser.has("api-key") ? parser.get<std::string>("api-key") : "";
    return config;
}

// Map score within [min, max] to a red(low)->green(high) truecolor escape.
std::string gradientColor(float score, float min_score, float max_score)
{
    float t = 0.5f;
    if (max_score > min_score)
        t = (score - min_score) / (max_score - min_score);
    t = std::clamp(t, 0.0f, 1.0f);

    const int red = static_cast<int>((1.0f - t) * 255.0f);
    const int green = static_cast<int>(t * 255.0f);
    return "\x1b[38;2;" + std::to_string(red) + ";" + std::to_string(green) + ";0m";
}

std::string ellipsize(const std::string& text, size_t max_length)
{
    if (text.size() <= max_length)
        return text;
    return text.substr(0, max_length - 1) + "\u2026";
}

std::string trimLeading(const std::string& text)
{
    size_t start = 0;
    while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start])))
        ++start;
    return text.substr(start);
}

class Store
{
public:
    // Fill the lowest empty slot and return its index.
    size_t add(std::string text, EmbeddingVector vector)
    {
        Entry entry { std::move(text), std::move(vector) };
        for (size_t i = 0; i < m_slots.size(); ++i)
        {
            if (!m_slots[i].has_value())
            {
                m_slots[i] = std::move(entry);
                return i;
            }
        }
        m_slots.push_back(std::move(entry));
        return m_slots.size() - 1;
    }

    bool remove(size_t index)
    {
        if (index >= m_slots.size() || !m_slots[index].has_value())
            return false;
        m_slots[index].reset();
        return true;
    }

    const Entry* get(size_t index) const
    {
        if (index >= m_slots.size() || !m_slots[index].has_value())
            return nullptr;
        return &m_slots[index].value();
    }

    size_t capacity() const { return m_slots.size(); }

    size_t count() const
    {
        size_t total = 0;
        for (const auto& slot : m_slots)
        {
            if (slot.has_value())
                ++total;
        }
        return total;
    }

    void clear() { m_slots.clear(); }

private:
    std::vector<std::optional<Entry>> m_slots;
};

void printHelp()
{
    std::cout
        << "Commands:\n"
        << "  add <text>     Embed text and store it in the lowest empty slot\n"
        << "  list           List stored entries with their slot numbers\n"
        << "  show <i>       Pretty-print the raw float values of slot i\n"
        << "  remove <i>     Clear slot i (slot numbers are kept, no shifting)\n"
        << "  comp-dot <i>   Rank other entries by dot product with slot i\n"
        << "  comp-cos <i>   Rank other entries by cosine similarity with slot i\n"
        << "  samples        Embed a built-in set of sample sentences\n"
        << "  clear          Remove all entries\n"
        << "  help           Show this help\n"
        << "  quit / exit    Quit\n";
}

void doList(const Store& store)
{
    if (store.count() == 0)
    {
        std::cout << kDim << "(empty)" << kReset << "\n";
        return;
    }

    for (size_t i = 0; i < store.capacity(); ++i)
    {
        const Entry* entry = store.get(i);
        if (!entry)
            continue;
        std::cout << kBold << "[" << i << "]" << kReset << " "
                  << ellipsize(entry->text, 60) << "\n";
    }
}

void doShow(const Store& store, size_t index)
{
    const Entry* entry = store.get(index);
    if (!entry)
    {
        std::cout << "Slot [" << index << "] is empty.\n";
        return;
    }

    const EmbeddingVector& vector = entry->vector;
    std::cout << kBold << "[" << index << "]" << kReset << " " << ellipsize(entry->text, 60) << "\n";
    std::cout << kDim << "dim=" << vector.size() << "  norm=" << vector.norm() << kReset << "\n";

    constexpr size_t kPerRow = 8;
    char buffer[16];
    for (size_t i = 0; i < vector.size(); i += kPerRow)
    {
        std::snprintf(buffer, sizeof(buffer), "%4zu", i);
        std::cout << kDim << "  " << buffer << " |" << kReset;

        const size_t row_end = std::min(i + kPerRow, vector.size());
        for (size_t j = i; j < row_end; ++j)
        {
            std::snprintf(buffer, sizeof(buffer), " %+9.4f", vector[j]);
            std::cout << buffer;
        }
        std::cout << "\n";
    }
}

void doComp(const Store& store, size_t index, Metric metric)
{
    const Entry* base = store.get(index);
    if (!base)
    {
        std::cout << "Slot [" << index << "] is empty.\n";
        return;
    }

    struct Row
    {
        size_t index;
        float score;
    };

    std::vector<Row> rows;
    for (size_t i = 0; i < store.capacity(); ++i)
    {
        if (i == index)
            continue;
        const Entry* other = store.get(i);
        if (!other)
            continue;

        const float score = metric == Metric::Dot
            ? base->vector.dotProduct(other->vector)
            : base->vector.similarity(other->vector);
        rows.push_back({i, score});
    }

    if (rows.empty())
    {
        std::cout << kDim << "(no other entries to compare)" << kReset << "\n";
        return;
    }

    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        return a.score > b.score;
    });

    const float min_score = rows.back().score;
    const float max_score = rows.front().score;
    const char* metric_name = metric == Metric::Dot ? "dot product" : "cosine similarity";

    std::cout << kBold << "[" << index << "]" << kReset << " "
              << ellipsize(base->text, 60) << " - ranked by " << metric_name << " (desc)\n";
    std::cout << kDim << "  slot   score       text" << kReset << "\n";

    char buffer[32];
    for (const Row& row : rows)
    {
        const Entry* other = store.get(row.index);

        std::snprintf(buffer, sizeof(buffer), "%-5d", static_cast<int>(row.index));
        std::cout << "  " << buffer << "  ";

        std::snprintf(buffer, sizeof(buffer), "%-10.4f", row.score);
        std::cout << gradientColor(row.score, min_score, max_score) << buffer << kReset;

        std::cout << "  " << ellipsize(other->text, 48) << "\n";
    }
}

void doSamples(EmbeddingClient& client, Store& store)
{
    const std::vector<std::string>& samples = sampleSentences();

    std::vector<std::string_view> views;
    views.reserve(samples.size());
    for (const std::string& sentence : samples)
        views.emplace_back(sentence);

    Spinner spinner("Embedding samples");
    spinner.start();
    std::vector<EmbeddingVector> vectors;
    const Result result = client.embedBatch(views, vectors);
    spinner.stop();

    if (result != Result::SUCCESS)
    {
        std::cerr << "embedBatch failed: " << resultToString(result) << "\n";
        return;
    }

    for (size_t i = 0; i < samples.size(); ++i)
    {
        const size_t index = store.add(samples[i], std::move(vectors[i]));
        std::cout << kBold << "[" << index << "]" << kReset << " " << samples[i] << "\n";
    }
    std::cout << samples.size() << " sample sentences embedded.\n";
}

bool parseIndex(const std::string& arg, size_t& out_index)
{
    if (arg.empty())
        return false;
    try
    {
        size_t consumed = 0;
        const long value = std::stol(arg, &consumed);
        if (consumed != arg.size() || value < 0)
            return false;
        out_index = static_cast<size_t>(value);
        return true;
    }
    catch (const std::exception&)
    {
        return false;
    }
}
}  // namespace

int main(int argc, const char* argv[])
{
    std::cout.setf(std::ios::unitbuf);

    ArgParser parser("test-text-embed", "Interactive embedding tester using ws::embedding::EmbeddingClient.");
    parser.addArgument("--host", "-H")
        .help("Ollama host or full base URL.")
        .defaultValue("127.0.0.1");
    parser.addArgument("--port", "-p")
        .help("Ollama port (ignored when --host includes a scheme).")
        .defaultValue("11434");
    parser.addArgument("--protocol")
        .help("Client protocol name (openai-ollama or openai).")
        .defaultValue("openai-ollama");
    parser.addArgument("--model", "-m")
        .help("Embedding model name.")
        .defaultValue("nomic-embed-text");
    parser.addArgument("--api-key", "-k")
        .help("API key (optional).");
    parser.addArgument("--ensure-model")
        .help("Call ensureModelLoaded() before starting.")
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

    EmbeddingClient client;
    std::string init_error;
    const Result init_result = client.init(buildModelConfig(parser), init_error);
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

    std::cout << "Connected to " << client.getHost()
              << " (model: " << client.getModel() << ")\n\n";
    printHelp();
    std::cout << "\n";

    Store store;
    std::string line;
    while (true)
    {
        std::cout << ">> " << std::flush;
        if (!std::getline(std::cin, line))
            break;

        const std::string trimmed = trimLeading(line);
        if (trimmed.empty())
            continue;

        std::string command;
        std::string rest;
        const size_t space = trimmed.find(' ');
        if (space == std::string::npos)
        {
            command = trimmed;
        }
        else
        {
            command = trimmed.substr(0, space);
            rest = trimLeading(trimmed.substr(space + 1));
        }

        if (command == "quit" || command == "exit")
            break;

        if (command == "help")
        {
            printHelp();
        }
        else if (command == "clear")
        {
            store.clear();
            std::cout << "All entries removed.\n";
        }
        else if (command == "list")
        {
            doList(store);
        }
        else if (command == "samples")
        {
            doSamples(client, store);
        }
        else if (command == "add")
        {
            if (rest.empty())
            {
                std::cout << "Usage: add <text>\n";
                continue;
            }

            Spinner spinner("Embedding");
            spinner.start();
            EmbeddingVector vector;
            const Result result = client.embed(rest, vector);
            spinner.stop();

            if (result != Result::SUCCESS)
            {
                std::cerr << "embed failed: " << resultToString(result) << "\n";
                continue;
            }

            const size_t index = store.add(rest, std::move(vector));
            std::cout << "[" << index << "] added (dim=" << store.get(index)->vector.size() << ")\n";
        }
        else if (command == "show")
        {
            size_t index = 0;
            if (!parseIndex(rest, index))
            {
                std::cout << "Usage: show <i>\n";
                continue;
            }
            doShow(store, index);
        }
        else if (command == "remove")
        {
            size_t index = 0;
            if (!parseIndex(rest, index))
            {
                std::cout << "Usage: remove <i>\n";
                continue;
            }
            std::cout << (store.remove(index)
                ? "[" + std::to_string(index) + "] removed\n"
                : "[" + std::to_string(index) + "] slot is empty\n");
        }
        else if (command == "comp-dot")
        {
            size_t index = 0;
            if (!parseIndex(rest, index))
            {
                std::cout << "Usage: comp-dot <i>\n";
                continue;
            }
            doComp(store, index, Metric::Dot);
        }
        else if (command == "comp-cos")
        {
            size_t index = 0;
            if (!parseIndex(rest, index))
            {
                std::cout << "Usage: comp-cos <i>\n";
                continue;
            }
            doComp(store, index, Metric::Cosine);
        }
        else
        {
            std::cout << "Unknown command: " << command << " (type help)\n";
        }
    }

    return 0;
}
