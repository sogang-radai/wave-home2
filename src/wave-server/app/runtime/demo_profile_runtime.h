#pragma once

#include "profile_runtime.h"

#include "../../demo/demo_alarms_facade.h"
#include "../../demo/demo_automation_runtime.h"
#include "../../demo/demo_chat_session_facade.h"
#include "../../demo/demo_devices_internal_facade.h"
#include "../../demo/demo_iot_facade.h"
#include "../../demo/demo_power_meter.h"
#include "../../demo/demo_rules_facade.h"
#include "../../demo/demo_schedule_tasks_facade.h"
#include "../../demo/demo_session_registry.h"

WAVE_NAMESPACE_BEGIN

class DemoProfileRuntime :
    public IProfileRuntime
{
public:
    ProfileKind kind() const override { return ProfileKind::Demo; }
    const char* name() const override { return "demo"; }

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

    DemoSessionRegistry* demoSessions() override { return &m_sessions; }
    DemoPowerMeter* demoPowerMeter() override { return &m_powerMeter; }

private:
    bool m_startedDemoAutomation = false;

    DemoSessionRegistry m_sessions;
    DemoPowerMeter m_powerMeter;
    DemoAutomationRuntime m_automation;

    DemoIotFacade m_iot;
    DemoDevicesInternalFacade m_devicesInternal;
    DemoAlarmsFacade m_alarms;
    DemoScheduleTasksFacade m_scheduleTasks;
    DemoRulesFacade m_rules;
    DemoChatSessionFacade m_chat;
};

WAVE_NAMESPACE_END
