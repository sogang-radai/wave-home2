#include "ir_trigger_bridge.h"

#include <algorithm>
#include <cmath>
#include <fstream>

#include "../app/app_state.h"
#include "../core/json.h"
#include "../core/logger.h"
#include "trigger_manager.h"

WAVE_NAMESPACE_BEGIN
SERVICE_NAMESPACE_BEGIN

namespace
{
    json load_ir_list()
    {
        const auto path = AppState::get().irListPath();
        std::ifstream in(path);
        json root;
        in >> root;
        return root;
    }

    double timing_distance(const std::vector<uint16_t>& a, const std::vector<uint16_t>& b)
    {
        const size_t count = std::min(a.size(), b.size());
        if (count == 0)
            return 1e9;

        double sum = 0.0;
        for (size_t i = 0; i < count; ++i)
            sum += std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i]));

        sum += static_cast<double>(std::max(a.size(), b.size()) - count) * 1000.0;
        return sum / static_cast<double>(count);
    }
}

std::string matchIrCommandId(const std::vector<uint16_t>& received_timings)
{
    if (received_timings.empty())
        return {};

    try
    {
        const json root = load_ir_list();
        if (!root.contains("commands") || !root["commands"].is_array())
            return {};

        std::string best_id;
        double best_distance = 1e9;

        for (const auto& entry : root["commands"])
        {
            if (!entry.contains("id") || !entry.contains("timings") || !entry["timings"].is_array())
                continue;

            std::vector<uint16_t> timings;
            for (const auto& value : entry["timings"])
            {
                if (value.is_number_unsigned())
                    timings.push_back(value.get<uint16_t>());
            }

            const double distance = timing_distance(received_timings, timings);
            if (distance < best_distance)
            {
                best_distance = distance;
                best_id = entry["id"].get<std::string>();
            }
        }

        if (best_distance <= 250.0)
            return best_id;
    }
    catch (const std::exception& e)
    {
        WLOG_WARN("IR matcher failed: {}", e.what());
    }

    return {};
}

void notifyIrReceived(const std::string& device_id, const std::vector<uint16_t>& received_timings)
{
    const std::string command_id = matchIrCommandId(received_timings);
    if (command_id.empty())
        return;

    auto& app = AppState::get();
    if (!app.automationReady())
        return;

    app.triggerManager().onIrReceived(device_id, command_id);
    WLOG_INFO("IR trigger: device={} command={}", device_id, command_id);
}

SERVICE_NAMESPACE_END
WAVE_NAMESPACE_END
