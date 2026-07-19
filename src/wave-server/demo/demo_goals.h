#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <json/json.h>

#include "core/coredefs.h"

WAVE_NAMESPACE_BEGIN

Json::Value demoListGoals(
    const std::string& runtime_id,
    int64_t user_id,
    const std::optional<std::string>& status = std::nullopt);

std::optional<Json::Value> demoGetGoal(
    const std::string& runtime_id,
    int64_t user_id,
    int64_t goal_id);

std::optional<Json::Value> demoCreateGoal(
    const std::string& runtime_id,
    int64_t user_id,
    const std::string& title,
    const std::string& category,
    std::string& error,
    std::string& field);

std::optional<Json::Value> demoUpdateGoalStatus(
    const std::string& runtime_id,
    int64_t user_id,
    int64_t goal_id,
    const std::string& status,
    std::string& error);

/** Cached session coaching, else agent job (no DB persist), else category fallback. */
std::optional<Json::Value> demoGetGoalCoaching(
    const std::string& runtime_id,
    int64_t user_id,
    int64_t goal_id,
    const std::string& agent_base_url,
    std::string& error);

std::optional<Json::Value> demoApplyGoalRecommendation(
    const std::string& runtime_id,
    int64_t user_id,
    int64_t goal_id,
    int64_t recommendation_id,
    std::string& error,
    std::string& field);

std::optional<Json::Value> demoUpdateGoalRecommendation(
    const std::string& runtime_id,
    int64_t user_id,
    int64_t goal_id,
    int64_t recommendation_id,
    bool approved,
    std::string& error);

WAVE_NAMESPACE_END
