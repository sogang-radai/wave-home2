#include "r4sn/iq_protocol.h"

#include <cstring>

namespace r4sn::iq
{
    std::size_t payloadSize(const ChirpMode mode)
    {
        switch (mode)
        {
        case ChirpMode::PerChirp:
            return kChirpCount * kComplexF32Size;
        case ChirpMode::Average:
        case ChirpMode::MaxAbs:
        case ChirpMode::FirstChirp:
            return kComplexF32Size;
        }
        return 0;
    }

    std::size_t requestBodySize(const std::size_t target_count)
    {
        return kRequestHeaderSize + target_count * kTargetSpecSize;
    }

    std::size_t responseBodySize(const std::span<const TargetSpec>& targets)
    {
        std::size_t total = kResponseHeaderSize;
        for (const TargetSpec& target : targets)
            total += payloadSize(target.chirp_mode);
        return total;
    }

    std::size_t rdmResponseBodySize(const std::size_t range_count, const std::size_t doppler_count)
    {
        return kRdmResponseHeaderSize + range_count * doppler_count * kComplexF32Size;
    }

    bool isValidChirpMode(const uint8_t value)
    {
        return value <= static_cast<uint8_t>(ChirpMode::FirstChirp);
    }

    bool isValidRequestType(const uint8_t value)
    {
        return value <= static_cast<uint8_t>(RequestType::RangeDopplerMap);
    }

    bool isValidVaCombineMode(const uint8_t value)
    {
        return value <= static_cast<uint8_t>(VaCombineMode::Beamform);
    }

    VaCombineMode targetIqVaModeFromWire(const uint8_t wire)
    {
        switch (wire)
        {
        case kTargetIqVaWireAverageAll:
            return VaCombineMode::AverageAll;
        case kTargetIqVaWireSingleVa:
            return VaCombineMode::SingleVa;
        case kTargetIqVaWireBeamform:
            return VaCombineMode::Beamform;
        case kTargetIqVaWireLegacyBeamform:
        default:
            return VaCombineMode::Beamform;
        }
    }

    std::string validateTargetIqOptions(const uint8_t va_wire, const uint8_t tile, const uint8_t sub_ant)
    {
        if (va_wire > kTargetIqVaWireBeamform)
            return "invalid target iq va_wire";
        const auto mode = targetIqVaModeFromWire(va_wire);
        if (mode == VaCombineMode::SingleVa)
        {
            if (tile >= 12)
                return "tile out of range";
            if (sub_ant >= 16)
                return "sub_ant out of range";
        }
        return {};
    }

    std::string validateRequest(const IqRequestHeader& header, const std::span<const TargetSpec>& targets)
    {
        if (header.magic != kRequestMagic)
            return "invalid request magic";
        if (header.version != kProtocolVersion && header.version != kProtocolVersion2)
            return "unsupported protocol version";
        if (header.target_count == 0 || header.target_count > kMaxTargets)
            return "target_count out of range";
        if (targets.size() != header.target_count)
            return "target payload size mismatch";
        for (const TargetSpec& target : targets)
            if (!isValidChirpMode(static_cast<uint8_t>(target.chirp_mode)))
                return "invalid chirp_mode";

        return {};
    }

    std::string validateRdmRequest(const RdmRequestSpec& spec)
    {
        if (!isValidChirpMode(static_cast<uint8_t>(spec.chirp_mode)))
            return "invalid chirp_mode";
        if (!isValidVaCombineMode(static_cast<uint8_t>(spec.va_combine_mode)))
            return "invalid va_combine_mode";
        if (spec.va_combine_mode == VaCombineMode::SingleVa)
        {
            if (spec.tile >= 12)
                return "tile out of range";
            if (spec.sub_ant >= 16)
                return "sub_ant out of range";
        }
        if (spec.distance_max_m < spec.distance_min_m)
            return "distance_max < distance_min";
        if (spec.velocity_max_mps < spec.velocity_min_mps)
            return "velocity_max < velocity_min";
        return {};
    }

    bool readRequest(const std::span<const uint8_t>& data, IqRequest& out, std::string& error)
    {
        if (data.size() < kRequestHeaderSize)
        {
            error = "request too short for header";
            return false;
        }

        std::memcpy(&out.header, data.data(), kRequestHeaderSize);
        const std::size_t expected = requestBodySize(out.header.target_count);

        if (data.size() < expected)
        {
            error = "request too short for targets";
            return false;
        }

        out.targets.resize(out.header.target_count);
        const auto* target_bytes = data.data() + kRequestHeaderSize;
        std::memcpy(out.targets.data(), target_bytes, out.targets.size() * kTargetSpecSize);

        error = validateRequest(out.header, out.targets);
        return error.empty();
    }

    bool readRequestV2(
        const std::span<const uint8_t>& data,
        RequestType& type,
        IqRequest& iq_out,
        RdmRequestSpec& rdm_out,
        std::string& error)
    {
        if (data.size() < kRequestHeaderV2Size)
        {
            error = "request too short for v2 header";
            return false;
        }

        RequestHeaderV2 header{};
        std::memcpy(&header, data.data(), kRequestHeaderV2Size);

        if (header.magic != kRequestMagic)
        {
            error = "invalid request magic";
            return false;
        }
        if (header.version != kProtocolVersion2)
        {
            error = "unsupported protocol version";
            return false;
        }
        if (!isValidRequestType(header.request_type))
        {
            error = "invalid request_type";
            return false;
        }

        type = static_cast<RequestType>(header.request_type);

        if (type == RequestType::TargetIq)
        {
            if (data.size() < kRequestHeaderV2Size + sizeof(TargetIqBodyV2))
            {
                error = "target iq v2 body too short";
                return false;
            }

            TargetIqBodyV2 body{};
            std::memcpy(&body, data.data() + kRequestHeaderV2Size, sizeof(body));

            iq_out.header.magic = header.magic;
            iq_out.header.version = header.version;
            iq_out.header.target_count = body.target_count;

            const std::size_t expected = kRequestHeaderV2Size + sizeof(TargetIqBodyV2)
                                         + body.target_count * kTargetSpecSize;
            if (data.size() < expected)
            {
                error = "target iq v2 payload too short";
                return false;
            }

            iq_out.targets.resize(body.target_count);
            std::memcpy(
                iq_out.targets.data(),
                data.data() + kRequestHeaderV2Size + sizeof(TargetIqBodyV2),
                iq_out.targets.size() * kTargetSpecSize);

            error = validateRequest(iq_out.header, iq_out.targets);
            return error.empty();
        }

        if (data.size() < kRequestHeaderV2Size + kRdmRequestSpecSize)
        {
            error = "rdm request too short";
            return false;
        }

        std::memcpy(&rdm_out, data.data() + kRequestHeaderV2Size, kRdmRequestSpecSize);
        error = validateRdmRequest(rdm_out);
        return error.empty();
    }

    std::vector<uint8_t> writeResponse(const IqResponse& response)
    {
        const std::size_t body_size = kResponseHeaderSize + response.payload.size() * kComplexF32Size;
        std::vector<uint8_t> buffer(body_size);
        std::memcpy(buffer.data(), &response.header, kResponseHeaderSize);

        if (!response.payload.empty())
        {
            std::memcpy(
                buffer.data() + kResponseHeaderSize,
                response.payload.data(),
                response.payload.size() * kComplexF32Size);
        }
        return buffer;
    }

    std::vector<uint8_t> writeRdmResponse(const RdmResponse& response)
    {
        const std::size_t body_size = kRdmResponseHeaderSize + response.payload.size() * kComplexF32Size;
        std::vector<uint8_t> buffer(body_size);
        std::memcpy(buffer.data(), &response.header, kRdmResponseHeaderSize);

        if (!response.payload.empty())
        {
            std::memcpy(
                buffer.data() + kRdmResponseHeaderSize,
                response.payload.data(),
                response.payload.size() * kComplexF32Size);
        }
        return buffer;
    }
}  // namespace r4sn::iq
