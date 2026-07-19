#pragma once

#include <drogon/HttpController.h>

#include "core/coredefs.h"

#include "../http_controller.h"

WAVE_NAMESPACE_BEGIN
WEB_NAMESPACE_BEGIN
namespace v1 {

class SettingsController :
    public drogon::HttpController<SettingsController>
{
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(SettingsController::getGeneral, "/api/v1/settings/general", drogon::Get);
    ADD_METHOD_TO(SettingsController::putGeneral, "/api/v1/settings/general", drogon::Put);
    ADD_METHOD_TO(SettingsController::getSleep, "/api/v1/settings/sleep", drogon::Get);
    ADD_METHOD_TO(SettingsController::putSleep, "/api/v1/settings/sleep", drogon::Put);
    ADD_METHOD_TO(SettingsController::listSounds, "/api/v1/settings/sounds", drogon::Get);
    ADD_METHOD_TO(SettingsController::listTtsSpeakers, "/api/v1/settings/tts-speakers", drogon::Get);
    ADD_METHOD_TO(SettingsController::listAiModels, "/api/v1/settings/ai-models", drogon::Get);
    ADD_METHOD_TO(SettingsController::getAiAgent, "/api/v1/settings/ai-agent", drogon::Get);
    ADD_METHOD_TO(SettingsController::putAiAgent, "/api/v1/settings/ai-agent", drogon::Put);
    METHOD_LIST_END

    void getGeneral(const HttpRequestPtr& req, HttpResponseCallback&& callback);
    void putGeneral(const HttpRequestPtr& req, HttpResponseCallback&& callback);

    void getSleep(const HttpRequestPtr& req, HttpResponseCallback&& callback);
    void putSleep(const HttpRequestPtr& req, HttpResponseCallback&& callback);

    void listSounds(const HttpRequestPtr& req, HttpResponseCallback&& callback);
    void listTtsSpeakers(const HttpRequestPtr& req, HttpResponseCallback&& callback);
    void listAiModels(const HttpRequestPtr& req, HttpResponseCallback&& callback);

    void getAiAgent(const HttpRequestPtr& req, HttpResponseCallback&& callback);
    void putAiAgent(const HttpRequestPtr& req, HttpResponseCallback&& callback);
};

} // namespace v1
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
