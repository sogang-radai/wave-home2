#pragma once

#include <cstddef>
#include <cstdint>

namespace wsp // wave-station protocol
{

    // ---------------------------------------------------------------------------
    // Constants
    // ---------------------------------------------------------------------------

    constexpr uint32_t kMagic              = 0x57535031;  // "WSP1"
    constexpr uint8_t  kProtoVer           = 1;
    constexpr size_t   kHeaderSize         = 16;
    constexpr uint32_t kMaxPayload         = 4096;
    constexpr uint32_t kDefaultSampleRate  = 16000;
    constexpr uint8_t  kDefaultFrameMs     = 20;
    constexpr uint16_t kTcpPort            = 41737;  // ESP32 listens; host connects

    // ---------------------------------------------------------------------------
    // Message types
    // ---------------------------------------------------------------------------

    enum class Type : uint16_t
    {
        // Audio (streaming)
        MicPCM       = 0x0101,
        MicComp      = 0x0102,
        SpkPCM       = 0x0111,
        SpkComp      = 0x0112,

        // IR
        IrReceive    = 0x0201,  // device -> host
        IrTransmit   = 0x0202,  // host -> device

        // Sensors
        AmbientLight = 0x0301,
        Temperature  = 0x0302,
        Humidity     = 0x0303,

        // Control plane
        Subscribe    = 0xF001,
        Unsubscribe  = 0xF002,
        Ack          = 0xF010,
        Error        = 0xF011,
        Heartbeat    = 0xF020,
    };

    // ---------------------------------------------------------------------------
    // Header flags
    // ---------------------------------------------------------------------------

    enum HeaderFlagBits : uint8_t
    {
        HeaderFlag_None            = 0x00,
        HeaderFlag_HasTimestampBit = 0x01,  // payload begins with TimestampPrefix
        HeaderFlag_LastFrameBit    = 0x02,  // audio/stream end marker
        HeaderFlag_KeyFrameBit     = 0x04,  // compressed audio: decoder reset/sync
    };

    // ---------------------------------------------------------------------------
    // Packet headers (both are 16 bytes)
    // ---------------------------------------------------------------------------

    #pragma pack(push, 1)

    struct ControlHeader
    {
        uint32_t magic;        // kMagic
        uint8_t  version;      // kProtoVer
        uint8_t  reserved;     // must be 0
        uint16_t type;         // Type (control plane)
        uint32_t payloadSize;  // body length in bytes
        uint32_t requestId;    // matched by Ack / Error
    };

    struct DataHeader
    {
        uint32_t magic;        // kMagic
        uint8_t  version;      // kProtoVer
        uint8_t  flags;        // HeaderFlagBits
        uint16_t type;         // Type (data plane)
        uint32_t payloadSize;  // full payload length (see layout below)
        uint32_t sequence;     // per-type counter for loss/order detection
    };

    static_assert(sizeof(ControlHeader) == kHeaderSize);
    static_assert(sizeof(DataHeader) == kHeaderSize);

    // ---------------------------------------------------------------------------
    // Payload layout (data plane)
    //
    //   [TimestampPrefix]  optional, present when HeaderFlag_HasTimestampBit is set
    //   [TypeBody]         fixed fields defined per Type
    //   [VariableData]     trailing bytes (pcm, encoded audio, IR raw, message, ...)
    //
    // payloadSize covers TimestampPrefix + TypeBody + VariableData.
    // ---------------------------------------------------------------------------

    struct TimestampPrefix
    {
        uint64_t timestampUs;  // microseconds since device boot
    };

    // ---------------------------------------------------------------------------
    // Subscribe / Unsubscribe
    // ---------------------------------------------------------------------------

    enum SubscribeOptionFlagBits : uint32_t
    {
        SubscribeOptionFlag_None             = 0,
        SubscribeOptionFlag_OnChangeOnlyBits = 1 << 0,  // sensor: emit only on change
        SubscribeOptionFlag_CompressedBits   = 1 << 1,  // audio: prefer MicComp / SpkComp
    };

    struct SubscribeBody
    {
        uint16_t targetType;  // Type (MicComp, AmbientLight, ...)
        uint16_t intervalMs;  // sensor: e.g. 1000; audio: 0 = max rate
        uint32_t options;     // SubscribeOptionFlagBits
    };

    struct UnsubscribeBody
    {
        uint16_t targetType;  // Type to stop
    };

    // ---------------------------------------------------------------------------
    // Control responses
    // ---------------------------------------------------------------------------

    struct AckBody
    {
        uint32_t requestId;
        uint8_t  status;      // 0 = ok
        uint8_t  reserved[3]; // must be 0
    };

    enum ErrorCode : int32_t
    {
        ErrorOk              = 0,
        ErrorUnsupported     = -1,
        ErrorBusy            = -2,
        ErrorInvalidPayload  = -3,
    };

    struct ErrorBody
    {
        uint32_t requestId;
        int32_t  code;        // ErrorCode
        // optional ASCII/UTF-8 message follows immediately after this struct.
        // messageLen = payloadSize - sizeof(ErrorBody)
    };

    // Heartbeat uses ControlHeader with payloadSize = 0 (no body).

    // ---------------------------------------------------------------------------
    // Audio
    //
    // MicPCM  / SpkPCM  -> AudioPCMBody  + pcm bytes
    // MicComp / SpkComp -> AudioCompBody + encoded bytes
    // ---------------------------------------------------------------------------

    enum class AudioCodec : uint8_t
    {
        PCM  = 0,  // used only with MicPCM / SpkPCM types
        Opus = 1,
    };

    struct AudioPCMBody
    {
        uint32_t sampleRate;     // 16000 | 32000 | 48000
        uint8_t  channels;       // 1 = mono
        uint8_t  bitsPerSample;  // 16
        uint16_t sampleCount;    // samples per channel in this frame
        // uint8_t pcmData[sampleCount * channels * (bitsPerSample / 8)];
    };

    struct AudioCompBody
    {
        uint8_t  codec;           // AudioCodec::Opus
        uint32_t sampleRate;      // 16000 | 32000 | 48000
        uint8_t  channels;        // 1 = mono
        uint8_t  frameDurationMs; // 20 | 40 | 60 ...
        uint16_t encodedSize;
        // uint8_t encodedData[encodedSize];
    };

    // ---------------------------------------------------------------------------
    // IR (raw timing arrays in microseconds)
    // ---------------------------------------------------------------------------

    struct IrReceiveBody
    {
        uint16_t length;     // mark/space count (usually odd; starts with mark)
        uint8_t  overflow;   // 0 | 1 (IR library raw buffer overflow)
        uint8_t  reserved;   // must be 0
        // uint16_t rawData[length];  // microseconds
    };

    struct IrTransmitBody
    {
        uint16_t length;      // mark/space count
        uint32_t carrierHz;   // 38000 default; also 36000, 40000, ...
        uint16_t repeat;      // 0 = send once; N = send (N + 1) times
        // uint16_t rawData[length];  // microseconds, mark/space alternation
    };

    // ---------------------------------------------------------------------------
    // Sensors (AmbientLight, Temperature, Humidity)
    // ---------------------------------------------------------------------------

    enum class SensorUnit : uint8_t
    {
        UnitLux     = 1,  // AmbientLight
        UnitCelsius = 2,  // Temperature
        UnitPercent = 3,  // Humidity (0..100)
    };

    struct SensorBody
    {
        uint8_t  unit;       // SensorUnit
        uint8_t  quality;    // 0 = invalid, 255 = good
        uint16_t reserved;   // must be 0
        float    value;      // IEEE 754 little-endian
    };

    #pragma pack(pop)

    // ---------------------------------------------------------------------------
    // Payload size helpers (fixed body sizes; variable tail excluded)
    // ---------------------------------------------------------------------------

    constexpr size_t kTimestampPrefixSize = sizeof(TimestampPrefix);
    constexpr size_t kSubscribeBodySize   = sizeof(SubscribeBody);
    constexpr size_t kUnsubscribeBodySize = sizeof(UnsubscribeBody);
    constexpr size_t kAckBodySize           = sizeof(AckBody);
    constexpr size_t kErrorBodyFixedSize    = sizeof(ErrorBody);
    constexpr size_t kAudioPCMBodySize      = sizeof(AudioPCMBody);
    constexpr size_t kAudioCompBodySize     = sizeof(AudioCompBody);
    constexpr size_t kIrReceiveBodySize     = sizeof(IrReceiveBody);
    constexpr size_t kIrTransmitBodySize    = sizeof(IrTransmitBody);
    constexpr size_t kSensorBodySize        = sizeof(SensorBody);

    inline size_t pcm_data_size(const AudioPCMBody& body)
    {
        return static_cast<size_t>(body.sampleCount) * body.channels * (body.bitsPerSample / 8);
    }

    inline size_t ir_raw_data_size(uint16_t length)
    {
        return static_cast<size_t>(length) * sizeof(uint16_t);
    }

    inline size_t error_msg_size(uint32_t payloadSize)
    {
        return payloadSize > kErrorBodyFixedSize
            ? payloadSize - kErrorBodyFixedSize
            : 0;
    }

}  // namespace wsp
