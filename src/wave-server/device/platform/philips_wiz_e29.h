#pragma once

#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <string>

#include "../device.h"

WAVE_NAMESPACE_BEGIN
DEVICE_NAMESPACE_BEGIN

// Philips WiZ E29 smart bulb (color or tunable/dimmable white).
//
// Controlled over the WiZ local UDP protocol (JSON datagrams on port 38899).
// A single class ("philips_wiz_e29") serves the color, tunable-white and
// dimmable-white variants; the concrete capabilities are probed from the bulb
// at init time and can be inspected via getCapabilities() or the
// "capabilities" query.
class PhilipsWizE29 :
    public Device,
    public Queryable,
    public Actionable
{
public:
    struct Config
    {
        std::string className; // "philips_wiz_e29"
        std::string host;
        std::string mac;
        uint16_t port = 38899;
    };

    struct Capabilities
    {
        bool dimming = true;        // every WiZ bulb supports dimming
        bool color = false;         // RGB channels
        bool tunableWhite = false;  // adjustable color temperature
        uint16_t tempMinK = 0;      // valid when tunableWhite
        uint16_t tempMaxK = 0;
        std::string module;         // WiZ moduleName reported by the bulb
    };

    PhilipsWizE29();
    ~PhilipsWizE29() override;

    const Config& getConfig() const;
    const Capabilities& getCapabilities() const;

    // Device
    int init(const json& config) override;
    void shutdown() override;
    std::string_view getClass() const override;

    // Queryable
    json query(std::string_view name, const json& params) override;
    std::future<json> queryAsync(std::string_view name, const json& params, uint32_t timeout_ms = 1000) override;

    // Actionable
    int invoke(std::string_view name, const json& params) override;
    std::future<int> invokeAsync(std::string_view name, const json& params, uint32_t timeout_ms = 1000) override;

private:
    struct Impl;

    void registerActionsAndQueries();

    int setPower(bool on);
    int togglePower();
    int setBrightness(uint8_t percent);              // WiZ dimming, clamped to 10..100
    int setColorRGB(uint8_t r, uint8_t g, uint8_t b); // requires color capability
    int setColorTemperature(uint16_t kelvin);         // requires tunableWhite capability

    std::unique_ptr<Impl> m_impl;
    Config m_config;
    Capabilities m_capabilities;
    mutable std::mutex m_mutex;
};

/*
================================================================================
Exposed queries and actions
================================================================================

Queries (Queryable::query) -- color/temperature entries appear only when the
probed capabilities allow them:

{
  "capabilities": {
    "description": "Hardware capabilities probed from the bulb",
    "params": {},
    "result": {
      "class": "philips_wiz_e29",
      "dimming": true,
      "color": true,
      "tunable_white": true,
      "temp_min_k": 2200,
      "temp_max_k": 6500,
      "module": "ESP01_SHRGB1C_31"
    }
  },
  "state": {
    "description": "Current power and brightness",
    "params": {},
    "result": { "on": true, "brightness": 80 }
  },
  "brightness": {
    "description": "Current dimming level in percent",
    "params": {},
    "result": { "value": 80, "unit": "%" }
  },
  "color": {                                    // color capability only
    "description": "Current RGB color",
    "params": {},
    "result": { "r": 255, "g": 120, "b": 0 }
  },
  "temperature": {                              // tunableWhite capability only
    "description": "Current white color temperature",
    "params": {},
    "result": { "value": 2700, "unit": "K" }
  },
  "status": {
    "description": "All readable pilot fields reported by the bulb",
    "params": {},
    "result": {
      "on": true, "brightness": 80,
      "raw": { "state": true, "dimming": 80, "sceneId": 0,
               "temp": 2700, "r": 255, "g": 120, "b": 0, "rssi": -55 }
    }
  }
}

Actions (Actionable::invoke) -- color/temperature entries appear only when the
probed capabilities allow them:

{
  "on":     { "attributes": ["Stateful"],          "description": "Turn the bulb on",  "params": {} },
  "off":    { "attributes": ["Stateful"],          "description": "Turn the bulb off", "params": {} },
  "toggle": { "attributes": ["Toggle","Stateful"], "description": "Toggle power",      "params": {} },
  "brightness": {
    "attributes": ["Stateful"],
    "description": "Set dimming level (10-100)",
    "params": { "value": { "type": "integer", "min": 10, "max": 100, "required": true } }
  },
  "color": {                                    // color capability only
    "attributes": ["Stateful"],
    "description": "Set RGB color (0-255 per channel)",
    "params": {
      "r": { "type": "integer", "min": 0, "max": 255, "required": true },
      "g": { "type": "integer", "min": 0, "max": 255, "required": true },
      "b": { "type": "integer", "min": 0, "max": 255, "required": true }
    }
  },
  "temperature": {                              // tunableWhite capability only
    "attributes": ["Stateful"],
    "description": "Set white color temperature in kelvin",
    "params": { "value": { "type": "integer", "min": 2200, "max": 6500, "required": true } }
  }
}

================================================================================
Underlying WiZ local protocol (UDP JSON, port 38899)
================================================================================

Requests:

  on           -> {"method":"setPilot","params":{"state":true}}
  off          -> {"method":"setPilot","params":{"state":false}}
  brightness   -> {"method":"setPilot","params":{"dimming":80}}          // 10..100
  color        -> {"method":"setPilot","params":{"r":255,"g":120,"b":0}} // color model
  temperature  -> {"method":"setPilot","params":{"temp":2700}}           // tunable white
  read state   -> {"method":"getPilot","params":{}}
  read config  -> {"method":"getSystemConfig","params":{}}               // yields "mac","moduleName"

Responses:

  setPilot        <- {"method":"setPilot","env":"pro","result":{"success":true}}
  getPilot        <- {"method":"getPilot","env":"pro","result":{
                        "mac":"9877d5d0b442","state":true,"sceneId":0,
                        "dimming":80,"temp":2700,"r":255,"g":120,"b":0,"rssi":-55}}
  getSystemConfig <- {"result":{"mac":"9877d5d0b442","moduleName":"ESP01_SHRGB1C_31","fwVersion":"1.x"}}
*/

DEVICE_NAMESPACE_END
WAVE_NAMESPACE_END
