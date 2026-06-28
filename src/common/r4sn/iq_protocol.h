#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace r4sn::iq
{
    constexpr uint32_t kRequestMagic = 0x51495249u;   // "IIRQ"
    constexpr uint32_t kResponseMagic = 0x51535249u;  // "IIRS"
    constexpr uint32_t kPcFrameMagic = 0xABCD4321u;
    constexpr uint16_t kProtocolVersion = 1;
    constexpr uint16_t kProtocolVersion2 = 2;
    constexpr std::size_t kMaxTargets = 64;
    constexpr std::size_t kChirpCount = 64;
    constexpr std::size_t kMaxRdmCells = 256 * 64;

    enum class ChirpMode : uint8_t
    {
        PerChirp = 0,   // all 64 chirps / full Doppler FFT for RDM
        Average = 1,
        MaxAbs = 2,
        FirstChirp = 3,
    };

    enum class RequestType : uint8_t
    {
        TargetIq = 0,
        RangeDopplerMap = 1,
    };

    // RDM virtual-aperture combine (Doppler cube @ 0x8CA00000 only).
    enum class VaCombineMode : uint8_t
    {
        AverageAll = 0,   // complex mean over 12×16 channels
        SingleVa = 1,     // one tile + sub_ant (TX-RX pair)
        Beamform = 2,     // steered sum using az/el
    };

    enum class IqStatus : uint32_t
    {
        Ok = 0,
        InvalidRequest = 1,
        SyncTimeout = 2,
        InternalError = 3,
    };

    #pragma pack(push, 1)
    struct ComplexF32
    {
        float i;
        float q;
    };

    struct TargetSpec
    {
        float azimuth_rad;
        float elevation_rad;
        float distance_m;
        ChirpMode chirp_mode;
        uint8_t reserved[3]{};
    };

    struct IqRequestHeader
    {
        uint32_t magic;
        uint16_t version;
        uint16_t target_count;
    };

    struct RequestHeaderV2
    {
        uint32_t magic;
        uint16_t version;
        uint8_t request_type;
        uint8_t reserved;
    };

    struct TargetIqBodyV2
    {
        uint16_t target_count;
        uint8_t tile;     // SingleVa: TX tile 0..11
        uint8_t sub_ant;  // SingleVa: RX sub 0..15
    };

    // RequestHeaderV2.reserved wire values for TargetIq (legacy clients send 0 → beamform).
    constexpr uint8_t kTargetIqVaWireLegacyBeamform = 0;
    constexpr uint8_t kTargetIqVaWireAverageAll = 1;
    constexpr uint8_t kTargetIqVaWireSingleVa = 2;
    constexpr uint8_t kTargetIqVaWireBeamform = 3;

    struct RdmRequestSpec
    {
        float azimuth_rad;
        float elevation_rad;
        float distance_min_m;
        float distance_max_m;
        float velocity_min_mps;
        float velocity_max_mps;
        ChirpMode chirp_mode;
        VaCombineMode va_combine_mode;
        uint8_t tile;
        uint8_t sub_ant;
    };

    struct IqResponseHeader
    {
        uint32_t magic;
        uint16_t version;
        uint16_t target_count;
        uint32_t status;
    };

    struct RdmResponseHeader
    {
        uint32_t magic;
        uint16_t version;
        uint16_t request_type;
        uint32_t status;
        uint16_t range_count;
        uint16_t doppler_count;
        float range_min_m;
        float range_step_m;
        float velocity_min_mps;
        float velocity_step_mps;
    };
    #pragma pack(pop)

    constexpr std::size_t kTargetSpecSize = sizeof(TargetSpec);
    constexpr std::size_t kRequestHeaderSize = sizeof(IqRequestHeader);
    constexpr std::size_t kRequestHeaderV2Size = sizeof(RequestHeaderV2);
    constexpr std::size_t kResponseHeaderSize = sizeof(IqResponseHeader);
    constexpr std::size_t kRdmResponseHeaderSize = sizeof(RdmResponseHeader);
    constexpr std::size_t kRdmRequestSpecSize = sizeof(RdmRequestSpec);
    constexpr std::size_t kComplexF32Size = sizeof(ComplexF32);

    std::size_t payloadSize(ChirpMode mode);
    std::size_t requestBodySize(std::size_t target_count);
    std::size_t responseBodySize(const std::span<const TargetSpec>& targets);
    std::size_t rdmResponseBodySize(std::size_t range_count, std::size_t doppler_count);

    bool isValidChirpMode(uint8_t value);
    bool isValidRequestType(uint8_t value);
    bool isValidVaCombineMode(uint8_t value);
    VaCombineMode targetIqVaModeFromWire(uint8_t wire);
    std::string validateTargetIqOptions(uint8_t va_wire, uint8_t tile, uint8_t sub_ant);
    std::string validateRequest(const IqRequestHeader& header, const std::span<const TargetSpec>& targets);
    std::string validateRdmRequest(const RdmRequestSpec& spec);

    struct IqRequest
    {
        IqRequestHeader header{};
        std::vector<TargetSpec> targets;
    };

    struct IqResponse
    {
        IqResponseHeader header{};
        std::vector<ComplexF32> payload;
    };

    struct RdmResponse
    {
        RdmResponseHeader header{};
        std::vector<ComplexF32> payload;
    };

    bool readRequest(const std::span<const uint8_t>& data, IqRequest& out, std::string& error);
    bool readRequestV2(const std::span<const uint8_t>& data, RequestType& type, IqRequest& iq_out, RdmRequestSpec& rdm_out, std::string& error);
    std::vector<uint8_t> writeResponse(const IqResponse& response);
    std::vector<uint8_t> writeRdmResponse(const RdmResponse& response);
}  // namespace r4sn::iq
