#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../../core/json.h"

#include "core/coredefs.h"

WAVE_NAMESPACE_BEGIN
SERVICE_NAMESPACE_BEGIN

struct SecondSample
{
    int32_t statusClass = -1;
    std::vector<float> statusScores;
    bool tossValid = false;
    int32_t tossClass = -1;
    float tossIndex = 0.0f;
    int32_t sampleCount = 0;
    bool connected = false;
};

struct SecondSnapshot
{
    std::string timeStart;
    std::string statusLabel;
    double asleepR = 0.0;
    float tossIndex = 0.0f;
    bool tossValid = false;
    std::string tossLabel;
    int32_t sampleCount = 0;
    bool connected = false;
};

struct MinuteStat
{
    std::string timeStart;
    std::string timeEnd;
    double coverage = 0.0;
    json statusRatio;
    double tossMean = 0.0;
    double tossMax = 0.0;
    double tossP90 = 0.0;
    int32_t tossEvents = 0;
    json tossRatio;
    double asleepR = 0.0;
    int32_t sampleCount = 0;
};

struct ThirtyMinStat
{
    std::string timeStart;
    std::string timeEnd;
    double coverage = 0.0;
    json statusRatio;
    double tossMean = 0.0;
    double tossMax = 0.0;
    double tossP90 = 0.0;
    int32_t tossEvents = 0;
    json tossRatio;
    std::string summaryText;
};

class SecondAggregator
{
public:
    void reset(const std::string& second_start);
    void addSample(const SecondSample& sample);
    bool flush(SecondSnapshot& out_snapshot);

    const std::string& currentSecondStart() const { return m_secondStart; }
    bool hasData() const { return m_hasData; }

private:
    std::string m_secondStart;
    bool m_hasData = false;
    std::vector<float> m_statusScoreSum;
    int32_t m_statusSampleCount = 0;
    double m_asleepRSum = 0.0;
    int32_t m_asleepRCount = 0;
    float m_tossIndexSum = 0.0f;
    int32_t m_tossIndexCount = 0;
    int32_t m_tossClassCounts[3] = {0, 0, 0};
    int32_t m_sampleCount = 0;
    int32_t m_connectedSamples = 0;
};

class MinuteAggregator
{
public:
    void reset(const std::string& minute_start);
    void addSecond(const SecondSnapshot& second);
    bool flush(MinuteStat& out_stat);

    const std::string& currentMinuteStart() const { return m_minuteStart; }
    bool hasData() const { return m_hasData; }

private:
    std::string m_minuteStart;
    bool m_hasData = false;
    int32_t m_statusCounts[3] = {0, 0, 0};
    int32_t m_statusWeightSum = 0;
    double m_asleepRSum = 0.0;
    int32_t m_asleepRCount = 0;
    std::vector<float> m_tossIndices;
    int32_t m_tossClassCounts[3] = {0, 0, 0};
    int32_t m_tossClassWeight = 0;
    int32_t m_tossEvents = 0;
    std::string m_prevTossLabel;
    bool m_prevTossValid = false;
    float m_prevTossIndex = 0.0f;
    int32_t m_sampleCount = 0;
};

class ThirtyMinAggregator
{
public:
    void reset(const std::string& window_start);
    void addMinute(const MinuteStat& minute);
    bool flush(ThirtyMinStat& out_stat);

    const std::string& currentWindowStart() const { return m_windowStart; }
    bool hasData() const { return m_hasData; }

private:
    std::string m_windowStart;
    bool m_hasData = false;
    double m_coverageSum = 0.0;
    int32_t m_minuteCount = 0;
    json m_statusRatioSum = json::object();
    json m_tossRatioSum = json::object();
    double m_tossMeanSum = 0.0;
    double m_tossMax = 0.0;
    std::vector<double> m_tossP90Values;
    int32_t m_tossEvents = 0;
    int32_t m_sampleCount = 0;
};

std::string floorToSecond(const std::string& timestamp);
std::string floorToMinute(const std::string& timestamp);
std::string minuteEnd(const std::string& minute_start);
std::string floorToThirtyMin(const std::string& timestamp);
std::string thirtyMinEnd(const std::string& window_start);
std::string buildSummaryTemplate(const ThirtyMinStat& stat);
std::string mondayOfWeek(const std::string& date);

SERVICE_NAMESPACE_END
WAVE_NAMESPACE_END
