#include "device_class_registry.h"

#include <unordered_map>

WAVE_NAMESPACE_BEGIN
namespace web {
namespace internal {
namespace
{
    Json::Value action(
        const char* name,
        const char* description,
        std::initializer_list<const char*> attributes,
        Json::Value params_schema = Json::Value(Json::objectValue))
    {
        Json::Value item;
        item["name"] = name;
        item["description"] = description;
        Json::Value attrs(Json::arrayValue);
        for (const auto* attr : attributes)
            attrs.append(attr);
        item["attributes"] = attrs;
        item["paramsSchema"] = std::move(params_schema);
        return item;
    }

    Json::Value query(const char* name, const char* description = "")
    {
        Json::Value item;
        item["name"] = name;
        if (description && description[0] != '\0')
            item["description"] = description;
        return item;
    }

    Json::Value trigger_kinds(std::initializer_list<const char*> kinds)
    {
        Json::Value out(Json::arrayValue);
        for (const auto* kind : kinds)
            out.append(kind);
        return out;
    }

    Json::Value triggerable_queries(std::initializer_list<const char*> names)
    {
        Json::Value out(Json::arrayValue);
        for (const auto* name : names)
            out.append(name);
        return out;
    }

    Json::Value power_actions()
    {
        Json::Value items(Json::arrayValue);
        items.append(action("on", "전원 켜기", {"Stateful"}));
        items.append(action("off", "전원 끄기", {"Stateful"}));
        items.append(action("toggle", "전원 토글", {"Toggle", "Stateful"}));
        return items;
    }

    Json::Value plug_capabilities(const char* class_name, const char* label)
    {
        Json::Value caps;
        caps["class"] = class_name;
        caps["label"] = label;
        caps["actions"] = power_actions();
        Json::Value queries(Json::arrayValue);
        queries.append(query("switch", "현재 on/off 상태"));
        queries.append(query("voltage", "AC 전압(V)"));
        queries.append(query("current", "전류(mA)"));
        queries.append(query("power", "순간 전력(W)"));
        queries.append(query("energy", "누적 에너지(kWh)"));
        queries.append(query("status", "전체 datapoint"));
        caps["queries"] = queries;
        caps["trigger_kinds"] = trigger_kinds({"device_state"});
        caps["triggerable_queries"] = triggerable_queries({"power", "voltage", "current"});
        return caps;
    }

    Json::Value repeat_count_params_schema()
    {
        Json::Value schema;
        Json::Value count;
        count["type"] = "integer";
        count["minimum"] = 1;
        count["maximum"] = 32;
        count["description"] = "키 입력 반복 횟수(기본 1). 예: 볼륨 10칸 → count=10";
        schema["count"] = count;
        return schema;
    }

    Json::Value tv_actions()
    {
        const auto repeat_schema = repeat_count_params_schema();
        Json::Value items(Json::arrayValue);
        items.append(action("on", "전원 켜기", {"Stateful"}));
        items.append(action("off", "전원 끄기", {"Stateful"}));
        items.append(action("toggle", "전원 토글", {"Toggle", "Stateful"}));
        items.append(action("volume_up", "볼륨 올리기", {"Repeat"}, repeat_schema));
        items.append(action("volume_down", "볼륨 내리기", {"Repeat"}, repeat_schema));
        items.append(action("mute", "음소거 토글", {"Toggle", "Stateful"}));
        items.append(action("channel_up", "채널 올리기", {"Repeat"}, repeat_schema));
        items.append(action("channel_down", "채널 내리기", {"Repeat"}, repeat_schema));
        items.append(action("open_app", "앱 실행", {}));
        items.append(action("nav_up", "방향 위", {}, repeat_schema));
        items.append(action("nav_down", "방향 아래", {}, repeat_schema));
        items.append(action("nav_left", "방향 왼쪽", {}, repeat_schema));
        items.append(action("nav_right", "방향 오른쪽", {}, repeat_schema));
        items.append(action("select", "선택(OK)", {}, repeat_schema));
        items.append(action("back", "뒤로가기", {}, repeat_schema));
        items.append(action("play_pause", "재생/일시정지", {}, repeat_schema));
        items.append(action("home", "홈 화면", {}, repeat_schema));
        items.append(action("input_source", "외부 입력 전환", {}, repeat_schema));
        return items;
    }

    Json::Value tv_capabilities(const char* class_name, const char* label)
    {
        Json::Value caps;
        caps["class"] = class_name;
        caps["label"] = label;
        caps["actions"] = tv_actions();
        Json::Value queries(Json::arrayValue);
        queries.append(query("state", "전원/볼륨/채널/실행 앱 상태"));
        caps["queries"] = queries;
        caps["trigger_kinds"] = trigger_kinds({});
        return caps;
    }

    Json::Value brightness_params_schema()
    {
        Json::Value schema;
        Json::Value value;
        value["type"] = "integer";
        value["minimum"] = 10;
        value["maximum"] = 100;
        value["description"] = "밝기 %(10-100). 예: {\"value\": 40}";
        schema["value"] = value;
        return schema;
    }

    Json::Value color_params_schema()
    {
        Json::Value schema;
        Json::Value channel;
        channel["type"] = "integer";
        channel["minimum"] = 0;
        channel["maximum"] = 255;
        schema["r"] = channel;
        schema["g"] = channel;
        schema["b"] = channel;
        schema["r"]["description"] = "빨강 0-255";
        schema["g"]["description"] = "초록 0-255";
        schema["b"]["description"] = "파랑 0-255. 예: {\"r\":255,\"g\":64,\"b\":0}";
        return schema;
    }

    Json::Value temperature_params_schema()
    {
        Json::Value schema;
        Json::Value value;
        value["type"] = "integer";
        value["minimum"] = 2200;
        value["maximum"] = 6500;
        value["description"] = "색온도 Kelvin(2200-6500). 예: {\"value\": 2700}";
        schema["value"] = value;
        return schema;
    }

    Json::Value light_color_capabilities()
    {
        Json::Value caps;
        caps["class"] = "philips_wiz_e29_color";
        caps["label"] = "WiZ 컬러 조명";
        Json::Value actions(Json::arrayValue);
        actions.append(action("on", "전원 켜기", {"Stateful"}));
        actions.append(action("off", "전원 끄기", {"Stateful"}));
        actions.append(action("toggle", "전원 토글", {"Toggle", "Stateful"}));
        actions.append(action("brightness", "밝기 설정(10-100)", {"Stateful"}, brightness_params_schema()));
        actions.append(action("color", "RGB 색상 설정", {"Stateful"}, color_params_schema()));
        actions.append(action("temperature", "색온도 설정(K)", {"Stateful"}, temperature_params_schema()));
        caps["actions"] = actions;
        Json::Value queries(Json::arrayValue);
        queries.append(query("capabilities", "프로브된 하드웨어 기능"));
        queries.append(query("state", "전원/밝기"));
        queries.append(query("brightness", "밝기(%)"));
        queries.append(query("color", "현재 RGB"));
        queries.append(query("temperature", "현재 색온도(K)"));
        queries.append(query("status", "전체 pilot 필드"));
        caps["queries"] = queries;
        caps["trigger_kinds"] = trigger_kinds({});
        return caps;
    }

    Json::Value light_white_capabilities()
    {
        Json::Value caps;
        caps["class"] = "philips_wiz_e29_white";
        caps["label"] = "WiZ 화이트 조명";
        Json::Value actions(Json::arrayValue);
        actions.append(action("on", "전원 켜기", {"Stateful"}));
        actions.append(action("off", "전원 끄기", {"Stateful"}));
        actions.append(action("toggle", "전원 토글", {"Toggle", "Stateful"}));
        actions.append(action("brightness", "밝기 설정(10-100)", {"Stateful"}, brightness_params_schema()));
        actions.append(action("temperature", "색온도 설정(K)", {"Stateful"}, temperature_params_schema()));
        caps["actions"] = actions;
        Json::Value queries(Json::arrayValue);
        queries.append(query("capabilities", "프로브된 하드웨어 기능"));
        queries.append(query("state", "전원/밝기"));
        queries.append(query("brightness", "밝기(%)"));
        queries.append(query("temperature", "현재 색온도(K)"));
        queries.append(query("status", "전체 pilot 필드"));
        caps["queries"] = queries;
        caps["trigger_kinds"] = trigger_kinds({});
        return caps;
    }

    const std::unordered_map<std::string, Json::Value>& registry()
    {
        static const std::unordered_map<std::string, Json::Value> kRegistry = {
            {"tuya_ep2h", plug_capabilities("tuya_ep2h", "스마트 플러그")},
            {"samsung_g7", tv_capabilities("samsung_g7", "Samsung TV")},
            {"tizen_tv", tv_capabilities("tizen_tv", "Tizen TV")},
            {"philips_wiz_e29_color", light_color_capabilities()},
            {"philips_wiz_e29_white", light_white_capabilities()},
            {"wave_station", [] {
                 Json::Value caps;
                 caps["class"] = "wave_station";
                 caps["label"] = "Wave Station";
                 Json::Value actions(Json::arrayValue);
                 actions.append(action("send_ir", "등록된 IR 커맨드 전송", {"Repeat"}));
                 actions.append(action("subscribe", "WSP1 스트림 구독", {}));
                 actions.append(action("unsubscribe", "구독 해제", {}));
                 caps["actions"] = actions;
                 Json::Value queries(Json::arrayValue);
                 queries.append(query("capabilities", "mic/speaker/IR/센서 플래그"));
                 queries.append(query("session", "host, port, 오디오 포맷"));
                 queries.append(query("status", "연결·구독 상태"));
                 queries.append(query("mic_level", "마이크 RMS 0..1"));
                 queries.append(query("env", "조도/온습도 스냅샷"));
                 queries.append(query("last_ir", "최근 IR 수신"));
                 caps["queries"] = queries;
                 caps["trigger_kinds"] = trigger_kinds({"ir_recv"});
                 return caps;
             }()},
            {"reolink_e1_pro", [] {
                 Json::Value caps;
                 caps["class"] = "reolink_e1_pro";
                 caps["label"] = "IoT 카메라";
                 caps["actions"] = Json::Value(Json::arrayValue);
                 Json::Value queries(Json::arrayValue);
                 queries.append(query("stream", "RTSP 스트림 URI"));
                 queries.append(query("status", "스트리밍/마이크 상태"));
                 caps["queries"] = queries;
                 caps["trigger_kinds"] = trigger_kinds({});
                 caps["ptz"] = true;
                 return caps;
             }()},
            {"droid_cam", [] {
                 Json::Value caps;
                 caps["class"] = "droid_cam";
                 caps["label"] = "폰 카메라";
                 caps["actions"] = Json::Value(Json::arrayValue);
                 Json::Value queries(Json::arrayValue);
                 queries.append(query("capabilities", "snapshot/stream/mic 플래그"));
                 queries.append(query("session", "HTTP 엔드포인트"));
                 queries.append(query("status", "연결·스트리밍 상태"));
                 queries.append(query("stream", "MJPEG source URI"));
                 caps["queries"] = queries;
                 caps["trigger_kinds"] = trigger_kinds({});
                 caps["ptz"] = false;
                 return caps;
             }()},
            {"srs_r4sn", [] {
                 Json::Value caps;
                 caps["class"] = "srs_r4sn";
                 caps["label"] = "mmWave 레이더";
                 caps["actions"] = Json::Value(Json::arrayValue);
                 Json::Value queries(Json::arrayValue);
                 queries.append(query("point_cloud", "포인트 클라우드 스트림"));
                 queries.append(query("iq", "IQ 샘플"));
                 caps["queries"] = queries;
                 caps["trigger_kinds"] = trigger_kinds({"gesture"});
                 return caps;
             }()},
        };
        return kRegistry;
    }
}

Json::Value DeviceClassRegistry::list_classes()
{
    Json::Value items(Json::arrayValue);
    for (const auto& [class_name, caps] : registry())
        items.append(caps);

    Json::Value body;
    body["items"] = items;
    body["count"] = static_cast<Json::UInt>(items.size());
    return body;
}

Json::Value DeviceClassRegistry::capabilities_for_class(const std::string& class_name)
{
    const auto& map = registry();
    const auto it = map.find(class_name);
    if (it != map.end())
        return it->second;

    Json::Value fallback;
    fallback["class"] = class_name;
    fallback["label"] = class_name;
    fallback["actions"] = Json::Value(Json::arrayValue);
    fallback["queries"] = Json::Value(Json::arrayValue);
    fallback["trigger_kinds"] = Json::Value(Json::arrayValue);
    return fallback;
}

std::string DeviceClassRegistry::label_for_class(const std::string& class_name)
{
    const auto caps = capabilities_for_class(class_name);
    return caps.get("label", class_name).asString();
}

} // namespace internal
} // namespace web
WAVE_NAMESPACE_END
