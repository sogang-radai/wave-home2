#include "sleep_aggregator.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <sstream>

WAVE_NAMESPACE_BEGIN
SERVICE_NAMESPACE_BEGIN

namespace
{
    constexpr double kCoverageSkipThreshold = 0.3;
    constexpr int32_t kExpectedSamplesPerMinute = 1200;

    std::tm parseLocalTm(const std::string& timestamp)
    {
        std::tm tm{};
        if (timestamp.size() >= 19)
        {
            tm.tm_year = std::stoi(timestamp.substr(0, 4)) - 1900;
            tm.tm_mon = std::stoi(timestamp.substr(5, 2)) - 1;
            tm.tm_mday = std::stoi(timestamp.substr(8, 2));
            tm.tm_hour = std::stoi(timestamp.substr(11, 2));
            tm.tm_min = std::stoi(timestamp.substr(14, 2));
            tm.tm_sec = std::stoi(timestamp.substr(17, 2));
        }
        return tm;
    }

    std::string formatTm(const std::tm& tm)
    {
        std::ostringstream oss;
        oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
        return oss.str();
    }

    std::string shiftTm(const std::tm& base, int field, int delta)
    {
        std::tm tm = base;
        if (field == 0)
            tm.tm_sec += delta;
        else if (field == 1)
            tm.tm_min += delta;
        else
            tm.tm_hour += delta;
        std::mktime(&tm);
        return formatTm(tm);
    }

    int32_t argmaxScores(const std::vector<float>& scores)
    {
        if (scores.empty())
            return -1;
        const auto it = std::max_element(scores.begin(), scores.end());
        return static_cast<int32_t>(std::distance(scores.begin(), it));
    }

    const char* statusLabel(int32_t status_class)
    {
        switch (status_class)
        {
        case 0: return "absent";
        case 1: return "awake";
        case 2: return "asleep";
        default: return "unknown";
        }
    }

    const char* tossLabel(int32_t toss_class)
    {
        switch (toss_class)
        {
        case 0: return "calm";
        case 1: return "slight";
        case 2: return "moderate";
        default: return "calm";
        }
    }

    json normalizeRatio(const json& counts)
    {
        double total = 0.0;
        for (auto it = counts.begin(); it != counts.end(); ++it)
            total += it.value().get<double>();

        json out = json::object();
        if (total <= 0.0)
            return out;

        for (auto it = counts.begin(); it != counts.end(); ++it)
            out[it.key()] = it.value().get<double>() / total;
        return out;
    }

    double percentile90(std::vector<double> values)
    {
        if (values.empty())
            return 0.0;
        std::sort(values.begin(), values.end());
        const size_t idx = static_cast<size_t>(std::ceil(values.size() * 0.9)) - 1;
        return values[std::min(idx, values.size() - 1)];
    }
}

std::string floorToSecond(const std::string& timestamp)
{
    if (timestamp.size() < 19)
        return timestamp;
    return timestamp.substr(0, 17) + "00";
}

std::string floorToMinute(const std::string& timestamp)
{
    if (timestamp.size() < 16)
        return timestamp;
    return timestamp.substr(0, 14) + "00:00";
}

std::string minuteEnd(const std::string& minute_start)
{
    auto tm = parseLocalTm(minute_start);
    return shiftTm(tm, 1, 1);
}

std::string floorToThirtyMin(const std::string& timestamp)
{
    auto tm = parseLocalTm(timestamp);
    tm.tm_sec = 0;
    tm.tm_min = (tm.tm_min / 30) * 30;
    return formatTm(tm);
}

std::string thirtyMinEnd(const std::string& window_start)
{
    auto tm = parseLocalTm(window_start);
    return shiftTm(tm, 1, 30);
}

std::string mondayOfWeek(const std::string& date)
{
    auto tm = parseLocalTm(date + " 00:00:00");
    const int wday = tm.tm_wday;
    const int diff = wday == 0 ? -6 : 1 - wday;
    return shiftTm(tm, 2, diff).substr(0, 10);
}

std::string buildSummaryTemplate(const ThirtyMinStat& stat)
{
    std::string asleep_pct = "0%";
    if (stat.statusRatio.is_object() && stat.statusRatio.contains("asleep"))
        asleep_pct = std::to_string(static_cast<int>(stat.statusRatio["asleep"].get<double>() * 100.0)) + "%";

    std::string calm_pct = "0%";
    if (stat.tossRatio.is_object() && stat.tossRatio.contains("calm"))
        calm_pct = std::to_string(static_cast<int>(stat.tossRatio["calm"].get<double>() * 100.0)) + "%";

    const auto start_short = stat.timeStart.size() >= 16 ? stat.timeStart.substr(11, 5) : stat.timeStart;
    const auto end_short = stat.timeEnd.size() >= 16 ? stat.timeEnd.substr(11, 5) : stat.timeEnd;

    return start_short + "~" + end_short + " asleep " + asleep_pct + ", toss calm " + calm_pct;
}

void SecondAggregator::reset(const std::string& second_start)
{
    m_secondStart = second_start;
    m_hasData = false;
    m_statusScoreSum.clear();
    m_statusSampleCount = 0;
    m_asleepRSum = 0.0;
    m_asleepRCount = 0;
    m_tossIndexSum = 0.0f;
    m_tossIndexCount = 0;
    m_tossClassCounts[0] = m_tossClassCounts[1] = m_tossClassCounts[2] = 0;
    m_sampleCount = 0;
    m_connectedSamples = 0;
}

void SecondAggregator::addSample(const SecondSample& sample)
{
    m_hasData = true;
    m_sampleCount += std::max(1, sample.sampleCount);
    if (sample.connected)
        m_connectedSamples += std::max(1, sample.sampleCount);

    if (!sample.statusScores.empty())
    {
        if (m_statusScoreSum.size() < sample.statusScores.size())
            m_statusScoreSum.resize(sample.statusScores.size(), 0.0f);

        const int32_t weight = std::max(1, sample.sampleCount);
        for (size_t i = 0; i < sample.statusScores.size(); ++i)
            m_statusScoreSum[i] += sample.statusScores[i] * static_cast<float>(weight);
        m_statusSampleCount += weight;

        if (sample.statusScores.size() > 2)
        {
            m_asleepRSum += sample.statusScores[2] * weight;
            m_asleepRCount += weight;
        }
    }

    if (sample.tossValid)
    {
        const int32_t weight = std::max(1, sample.sampleCount);
        m_tossIndexSum += sample.tossIndex * weight;
        m_tossIndexCount += weight;
        if (sample.tossClass >= 0 && sample.tossClass < 3)
            m_tossClassCounts[sample.tossClass] += weight;
    }
}

bool SecondAggregator::flush(SecondSnapshot& out_snapshot)
{
    if (!m_hasData)
        return false;

    out_snapshot.timeStart = m_secondStart;
    out_snapshot.sampleCount = m_sampleCount;
    out_snapshot.connected = m_connectedSamples > 0;

    const int32_t status_class = argmaxScores(m_statusScoreSum);
    out_snapshot.statusLabel = statusLabel(status_class);
    out_snapshot.asleepR = m_asleepRCount > 0 ? m_asleepRSum / static_cast<double>(m_asleepRCount) : 0.0;

    out_snapshot.tossValid = m_tossIndexCount > 0;
    out_snapshot.tossIndex = m_tossIndexCount > 0 ? m_tossIndexSum / static_cast<float>(m_tossIndexCount) : 0.0f;

    int32_t best_toss = 0;
    int32_t best_count = m_tossClassCounts[0];
    for (int32_t i = 1; i < 3; ++i)
    {
        if (m_tossClassCounts[i] > best_count)
        {
            best_count = m_tossClassCounts[i];
            best_toss = i;
        }
    }
    out_snapshot.tossLabel = tossLabel(best_toss);

    reset(m_secondStart);
    return true;
}

void MinuteAggregator::reset(const std::string& minute_start)
{
    m_minuteStart = minute_start;
    m_hasData = false;
    m_statusCounts[0] = m_statusCounts[1] = m_statusCounts[2] = 0;
    m_statusWeightSum = 0;
    m_asleepRSum = 0.0;
    m_asleepRCount = 0;
    m_tossIndices.clear();
    m_tossClassCounts[0] = m_tossClassCounts[1] = m_tossClassCounts[2] = 0;
    m_tossClassWeight = 0;
    m_tossEvents = 0;
    m_prevTossLabel.clear();
    m_prevTossValid = false;
    m_prevTossIndex = 0.0f;
    m_sampleCount = 0;
}

void MinuteAggregator::addSecond(const SecondSnapshot& second)
{
    m_hasData = true;
    m_sampleCount += std::max(1, second.sampleCount);

    int32_t status_idx = 1;
    if (second.statusLabel == "absent")
        status_idx = 0;
    else if (second.statusLabel == "asleep")
        status_idx = 2;

    const int32_t weight = std::max(1, second.sampleCount);
    m_statusCounts[status_idx] += weight;
    m_statusWeightSum += weight;

    if (second.asleepR > 0.0 || second.statusLabel == "asleep")
    {
        m_asleepRSum += second.asleepR;
        m_asleepRCount += 1;
    }

    if (second.tossValid && second.statusLabel == "asleep")
    {
        m_tossIndices.push_back(second.tossIndex);
        int32_t toss_idx = 0;
        if (second.tossLabel == "slight")
            toss_idx = 1;
        else if (second.tossLabel == "moderate")
            toss_idx = 2;
        m_tossClassCounts[toss_idx] += weight;
        m_tossClassWeight += weight;

        const bool crossed_moderate = second.tossLabel == "moderate" && m_prevTossLabel != "moderate";
        const bool crossed_index = m_prevTossValid && m_prevTossIndex < 0.5f && second.tossIndex >= 0.5f;
        if (crossed_moderate || crossed_index)
            ++m_tossEvents;

        m_prevTossLabel = second.tossLabel;
        m_prevTossValid = true;
        m_prevTossIndex = second.tossIndex;
    }
}

bool MinuteAggregator::flush(MinuteStat& out_stat)
{
    if (!m_hasData)
        return false;

    out_stat.timeStart = m_minuteStart;
    out_stat.timeEnd = minuteEnd(m_minuteStart);
    out_stat.sampleCount = m_sampleCount;
    out_stat.coverage = static_cast<double>(m_sampleCount) / static_cast<double>(kExpectedSamplesPerMinute);
    if (out_stat.coverage < kCoverageSkipThreshold)
    {
        reset(m_minuteStart);
        return false;
    }

    json status_counts = json::object();
    status_counts["absent"] = m_statusCounts[0];
    status_counts["awake"] = m_statusCounts[1];
    status_counts["asleep"] = m_statusCounts[2];
    out_stat.statusRatio = normalizeRatio(status_counts);
    out_stat.asleepR = m_asleepRCount > 0 ? m_asleepRSum / static_cast<double>(m_asleepRCount) : 0.0;

    if (!m_tossIndices.empty())
    {
        double sum = 0.0;
        out_stat.tossMax = m_tossIndices[0];
        for (float value : m_tossIndices)
        {
            sum += value;
            out_stat.tossMax = std::max(out_stat.tossMax, static_cast<double>(value));
        }
        out_stat.tossMean = sum / static_cast<double>(m_tossIndices.size());
        std::vector<double> sorted(m_tossIndices.begin(), m_tossIndices.end());
        out_stat.tossP90 = percentile90(std::move(sorted));
    }

    out_stat.tossEvents = m_tossEvents;

    if (m_tossClassWeight > 0)
    {
        json toss_counts = json::object();
        toss_counts["calm"] = m_tossClassCounts[0];
        toss_counts["slight"] = m_tossClassCounts[1];
        toss_counts["moderate"] = m_tossClassCounts[2];
        out_stat.tossRatio = normalizeRatio(toss_counts);
    }
    else
    {
        out_stat.tossRatio = json::object();
    }

    reset(m_minuteStart);
    return true;
}

void ThirtyMinAggregator::reset(const std::string& window_start)
{
    m_windowStart = window_start;
    m_hasData = false;
    m_coverageSum = 0.0;
    m_minuteCount = 0;
    m_statusRatioSum = json::object();
    m_tossRatioSum = json::object();
    m_tossMeanSum = 0.0;
    m_tossMax = 0.0;
    m_tossP90Values.clear();
    m_tossEvents = 0;
    m_sampleCount = 0;
}

void ThirtyMinAggregator::addMinute(const MinuteStat& minute)
{
    m_hasData = true;
    ++m_minuteCount;
    m_coverageSum += minute.coverage;
    m_sampleCount += minute.sampleCount;
    m_tossEvents += minute.tossEvents;
    m_tossMeanSum += minute.tossMean;
    m_tossMax = std::max(m_tossMax, minute.tossMax);
    if (minute.tossP90 > 0.0)
        m_tossP90Values.push_back(minute.tossP90);

    if (minute.statusRatio.is_object())
    {
        for (auto it = minute.statusRatio.begin(); it != minute.statusRatio.end(); ++it)
        {
            const double weight = minute.sampleCount > 0 ? minute.sampleCount : 1;
            m_statusRatioSum[it.key()] = m_statusRatioSum.value(it.key(), 0.0) + it.value().get<double>() * weight;
        }
    }

    if (minute.tossRatio.is_object())
    {
        for (auto it = minute.tossRatio.begin(); it != minute.tossRatio.end(); ++it)
        {
            const double weight = minute.sampleCount > 0 ? minute.sampleCount : 1;
            m_tossRatioSum[it.key()] = m_tossRatioSum.value(it.key(), 0.0) + it.value().get<double>() * weight;
        }
    }
}

bool ThirtyMinAggregator::flush(ThirtyMinStat& out_stat)
{
    if (!m_hasData || m_minuteCount == 0)
        return false;

    out_stat.timeStart = m_windowStart;
    out_stat.timeEnd = thirtyMinEnd(m_windowStart);
    out_stat.coverage = m_coverageSum / static_cast<double>(m_minuteCount);
    out_stat.statusRatio = normalizeRatio(m_statusRatioSum);
    out_stat.tossRatio = normalizeRatio(m_tossRatioSum);
    out_stat.tossMean = m_tossMeanSum / static_cast<double>(m_minuteCount);
    out_stat.tossMax = m_tossMax;
    out_stat.tossP90 = percentile90(m_tossP90Values);
    out_stat.tossEvents = m_tossEvents;
    out_stat.summaryText = buildSummaryTemplate(out_stat);

    reset(m_windowStart);
    return true;
}

SERVICE_NAMESPACE_END
WAVE_NAMESPACE_END
