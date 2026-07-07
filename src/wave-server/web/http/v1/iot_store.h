#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <json/json.h>

#include "../../../device/device_manager.h"
#include "core/coredefs.h"

WAVE_NAMESPACE_BEGIN
namespace dev {
class ReolinkE1Pro;
}
namespace web {
namespace v1 {

class IotStore
{
public:
    explicit IotStore(dev::DeviceManager& devices);

    bool devicesAvailable() const;

    Json::Value getSummary() const;
    Json::Value listDevices() const;

    Json::Value queryDevice(const std::string& external_id, const std::string& query_name, std::string& code) const;
    Json::Value invokeDevice(
        const std::string& external_id,
        const std::string& action_name,
        const Json::Value& params,
        std::string& code);

    bool reconnectDevice(const std::string& external_id, std::string& error, std::string& code);

    Json::Value listEvents(const std::string& device_id) const;
    void logEvent(
        const std::string& type,
        const std::string& device_id,
        const std::string& device_name,
        const std::string& message,
        const std::string& triggered_by,
        const Json::Value& detail);

    std::optional<Json::Value> queryPlugStatus(const std::string& external_id, bool force_refresh = false) const;
    std::vector<std::string> listPlugIds() const;

    Json::Value getCameraStream(const std::string& external_id, std::string& code) const;
    Json::Value setCameraStream(const std::string& external_id, bool streaming, std::string& code);
    bool exchangeCameraWebRtc(
        const std::string& external_id,
        const std::string& offer_sdp,
        std::string& answer_sdp,
        std::string& code);
    Json::Value getPtzCapabilities(const std::string& external_id, std::string& code) const;
    bool moveCameraPtz(const std::string& external_id, const Json::Value& vector, std::string& code);
    bool stopCameraPtz(const std::string& external_id, std::string& code);
    Json::Value zoomCameraPtz(const std::string& external_id, int delta, std::string& code);
    bool captureCameraSnapshot(
        const std::string& external_id,
        std::vector<uint8_t>& out_jpeg,
        std::string& occurred_at,
        std::string& code);
    bool sendDeviceTts(
        const std::string& external_id,
        const std::string& text,
        int speaker_id,
        float speed,
        std::string& code);

    bool openCameraMp4Stream(const std::string& external_id, std::string& stream_name, std::string& code);

    dev::Device* findDevice(const std::string& external_id) const;
    std::string connectionStatusForEntry(const dev::DeviceManifestEntry& entry) const;

private:
    dev::DeviceManager& m_devices;

    bool isConnected(const dev::Device* device) const;
    std::string connectionStatus(const dev::DeviceManifestEntry& entry) const;
    Json::Value normalizeState(const dev::Device* device, const nlohmann::json& raw) const;
    std::string stateSummary(
        const dev::DeviceManifestEntry& entry,
        const dev::Device* device,
        const Json::Value& state) const;
    std::string panelForClass(const std::string& class_name) const;
    std::string classLabel(const std::string& class_name) const;

    dev::ReolinkE1Pro* requireReolinkCamera(const std::string& external_id, std::string& code);
    const dev::ReolinkE1Pro* requireReolinkCamera(const std::string& external_id, std::string& code) const;

    static Json::Value toJsonValue(const nlohmann::json& value);
    static bool isQueryError(const nlohmann::json& value);
};

void logIotEvent(
    const std::string& type,
    const std::string& device_id,
    const std::string& device_name,
    const std::string& message,
    const std::string& triggered_by = "manual",
    const Json::Value& detail = Json::Value(Json::objectValue));

// Clears browser stream viewer state when a camera device is re-initialized.
void resetCameraStreamSession(const std::string& external_id);

// Loads TTS models at startup; safe to call repeatedly.
bool warmUpTtsService(std::string& error);

bool isTtsServiceReady();

// Runs synthesis + camera playback on the background task queue.
void queueDeviceTts(
    const std::string& external_id,
    const std::string& text,
    int speaker_id,
    float speed);

// Releases camera streams, TTS runtime, and go2rtc before process exit.
void shutdownBackgroundServices();

} // namespace v1
} // namespace web
WAVE_NAMESPACE_END
