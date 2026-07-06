#include "settings_controller.h"

#include "../../../app/app_state.h"
#include "session_store.h"
#include "settings_store.h"

WAVE_NAMESPACE_BEGIN
WEB_NAMESPACE_BEGIN
namespace v1 {
namespace
{
    void requireDbAndActiveUser(
        const drogon::HttpRequestPtr& req,
        const std::function<void(const drogon::HttpResponsePtr&)>& callback,
        const std::function<void(int64_t user_id)>& on_ready)
    {
        auto& state = AppState::get();
        if (!state.db())
        {
            respondError(callback, 503, "DB_UNAVAILABLE", "데이터베이스를 사용할 수 없습니다.");
            return;
        }

        SessionStore sessions(state.db());
        SettingsStore settings(state.db());
        const auto user_id = settings.resolveActiveUserId(sessions, req);
        if (!user_id)
        {
            respondError(callback, 409, "ACTIVE_ACCOUNT_REQUIRED", "활성 구성원을 먼저 선택해주세요.");
            return;
        }

        on_ready(*user_id);
    }
}

void SettingsController::getGeneral(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    requireDbAndActiveUser(req, callback, [&](int64_t user_id)
    {
        SettingsStore store(AppState::get().db());
        callback(drogon::HttpResponse::newHttpJsonResponse(store.getGeneralSettings(user_id)));
    });
}

void SettingsController::putGeneral(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    const auto json = req->getJsonObject();
    if (!json || !json->isObject())
    {
        respondError(callback, 400, "INVALID_BODY", "JSON 본문이 필요합니다.");
        return;
    }

    requireDbAndActiveUser(req, callback, [&](int64_t user_id)
    {
        SettingsStore store(AppState::get().db());
        Json::Value saved;
        std::string error;
        std::string field;
        if (!store.putGeneralSettings(user_id, *json, saved, error, field))
        {
            respondError(callback, 400, field.empty() ? "INVALID_BODY" : "INVALID_SPEAKER", error);
            return;
        }
        callback(drogon::HttpResponse::newHttpJsonResponse(saved));
    });
}

void SettingsController::getSleep(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    requireDbAndActiveUser(req, callback, [&](int64_t user_id)
    {
        SettingsStore store(AppState::get().db());
        callback(drogon::HttpResponse::newHttpJsonResponse(store.getSleepConfig(user_id)));
    });
}

void SettingsController::putSleep(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    const auto json = req->getJsonObject();
    if (!json || !json->isObject())
    {
        respondError(callback, 400, "INVALID_BODY", "JSON 본문이 필요합니다.");
        return;
    }

    requireDbAndActiveUser(req, callback, [&](int64_t user_id)
    {
        SettingsStore store(AppState::get().db());
        Json::Value saved;
        std::string error;
        std::string field;
        if (!store.putSleepConfig(user_id, *json, saved, error, field))
        {
            const auto code = field == "bedtime" ? "INVALID_TIME_RANGE" : "VALIDATION_ERROR";
            respondError(callback, 400, code, error);
            return;
        }
        callback(drogon::HttpResponse::newHttpJsonResponse(saved));
    });
}

void SettingsController::listSounds(
    const drogon::HttpRequestPtr& /*req*/,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    auto& state = AppState::get();
    if (!state.db())
    {
        respondError(callback, 503, "DB_UNAVAILABLE", "데이터베이스를 사용할 수 없습니다.");
        return;
    }

    SettingsStore store(state.db());
    callback(drogon::HttpResponse::newHttpJsonResponse(store.listSounds()));
}

void SettingsController::listTtsSpeakers(
    const drogon::HttpRequestPtr& /*req*/,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    auto& state = AppState::get();
    if (!state.db())
    {
        respondError(callback, 503, "DB_UNAVAILABLE", "데이터베이스를 사용할 수 없습니다.");
        return;
    }

    SettingsStore store(state.db());
    callback(drogon::HttpResponse::newHttpJsonResponse(store.listTtsSpeakers()));
}

void SettingsController::listAiModels(
    const drogon::HttpRequestPtr& /*req*/,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    auto& state = AppState::get();
    if (!state.db())
    {
        respondError(callback, 503, "DB_UNAVAILABLE", "데이터베이스를 사용할 수 없습니다.");
        return;
    }

    SettingsStore store(state.db());
    callback(drogon::HttpResponse::newHttpJsonResponse(store.listAiModels()));
}

void SettingsController::getAiAgent(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    requireDbAndActiveUser(req, callback, [&](int64_t user_id)
    {
        SettingsStore store(AppState::get().db());
        callback(drogon::HttpResponse::newHttpJsonResponse(store.getAiAgentSettings(user_id)));
    });
}

void SettingsController::putAiAgent(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    const auto json = req->getJsonObject();
    if (!json || !json->isObject())
    {
        respondError(callback, 400, "INVALID_BODY", "JSON 본문이 필요합니다.");
        return;
    }

    requireDbAndActiveUser(req, callback, [&](int64_t user_id)
    {
        SettingsStore store(AppState::get().db());
        Json::Value saved;
        std::string error;
        std::string field;
        if (!store.putAiAgentSettings(user_id, *json, saved, error, field))
        {
            respondError(callback, 400, "INVALID_MODEL", error);
            return;
        }
        callback(drogon::HttpResponse::newHttpJsonResponse(saved));
    });
}

} // namespace v1
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
