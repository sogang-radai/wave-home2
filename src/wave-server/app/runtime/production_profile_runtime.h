#pragma once

#include "profile_runtime.h"

#include "../../facade/real_alarms_facade.h"
#include "../../facade/real_chat_session_facade.h"
#include "../../facade/real_devices_internal_facade.h"
#include "../../facade/real_iot_facade.h"
#include "../../facade/real_rules_facade.h"
#include "../../facade/real_schedule_tasks_facade.h"

WAVE_NAMESPACE_BEGIN

class ProductionProfileRuntime :
    public IProfileRuntime
{
public:
    ProfileKind kind() const override { return ProfileKind::Production; }
    const char* name() const override { return "real"; }

    void applyConfigDefaults(AppConfig& config) const override;
    void startServices(AppState& app) override;
    void startPostListen(AppState& app) override;
    void onDatabaseReady(AppState& app, const db::DbClientPtr& client) override;
    void shutdown(AppState& app) override;

    facade::IIotFacade& iot() override { return m_iot; }
    facade::IDevicesInternalFacade& devicesInternal() override { return m_devicesInternal; }
    facade::IAlarmsFacade& alarms() override { return m_alarms; }
    facade::IScheduleTasksFacade& scheduleTasks() override { return m_scheduleTasks; }
    facade::IRulesFacade& rules() override { return m_rules; }
    facade::IChatSessionFacade& chat() override { return m_chat; }

private:
    bool m_startedDeviceStack = false;
    bool m_startedSleepManager = false;

    facade::RealIotFacade m_iot;
    facade::RealDevicesInternalFacade m_devicesInternal;
    facade::RealAlarmsFacade m_alarms;
    facade::RealScheduleTasksFacade m_scheduleTasks;
    facade::RealRulesFacade m_rules;
    facade::RealChatSessionFacade m_chat;
};

WAVE_NAMESPACE_END
