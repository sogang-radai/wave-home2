#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace iq
{
    constexpr uint32_t kRequestMagic = 0x51495249u;   // "IIRQ"
    constexpr uint32_t kResponseMagic = 0x51535249u;  // "IIRS"
    constexpr uint32_t kPcFrameMagic = 0xABCD4321u;
    constexpr uint32_t kProtocolVersion = 1;

    constexpr size_t kMaxTargets = 64;
    constexpr size_t kChirpCount = 64;
    constexpr size_t kMaxRdmCells = 256 * 64;
    constexpr size_t kTileCount = 12;
    constexpr size_t kSubAntCount = 16;

    constexpr uint64_t kChirpMaskFirst = 0x0000000000000001ull;
    constexpr uint64_t kChirpMaskAll = 0xffffffffffffffffull;

    enum class RequestType : uint8_t
    {
        Iq = 0,
        Rdm = 1,
    };

    enum class VaSelectMode : uint16_t
    {
        Single = 0,
        Mask = 1,
        List = 2,
    };

    enum class VaCombineMode : uint16_t
    {
        MultiVa = 0,
        AverageAll = 1,
        Beamform = 2,
    };

    enum class ChirpSelectMode : uint32_t
    {
        All = 0,
        Mask = 1,
    };

    enum class ChirpMode : uint32_t
    {
        Array = 0,
        Average = 1,
        MaxAbs = 2,
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

    struct RequestHeader
    {
        uint32_t magic;
        uint32_t version;
        uint32_t payload_size;
    };

    struct ResponseHeader
    {
        uint32_t magic;
        uint32_t version;
        uint32_t payload_size;
        uint32_t status;
    };

    /// Fixed IQ request fields. Tail: `target_count` × float distance (m), then optional blocks.
    struct IqRequest
    {
        RequestType type;
        uint8_t reserved0[3];
        ChirpMode chirp_mode;
        ChirpSelectMode chirp_select_mode;
        VaSelectMode va_select_mode;
        VaCombineMode va_combine_mode;
        uint8_t tile;
        uint8_t sub_ant;
        uint16_t target_count;
        float azimuth;
        float elevation;
    };

    struct RdmRequest
    {
        RequestType type;
        uint8_t reserved0[3];
        ChirpMode chirp_mode;
        ChirpSelectMode chirp_select_mode;
        VaSelectMode va_select_mode;
        VaCombineMode va_combine_mode;
        uint8_t tile;
        uint8_t sub_ant;
        uint16_t reserved1;
        float azimuth;
        float elevation;
        float distance_min;
        float distance_max;
        float velocity_min;
        float velocity_max;
    };

    struct OptChirpMask
    {
        uint64_t chirp_mask;
    };

    struct OptVaMask
    {
        uint16_t va_masks[kTileCount];
    };

    struct OptVaList
    {
        struct Item
        {
            uint8_t tile;
            uint8_t sub_ant;
        };

        uint16_t item_count;
    };

    struct IqResponseInfo
    {
        uint16_t target_count;
        uint16_t reserved;
    };

    struct RdmResponseInfo
    {
        uint16_t range_count;
        uint16_t doppler_count;
        float range_min_m;
        float range_step_m;
        float velocity_min_mps;
        float velocity_step_mps;
    };

#pragma pack(pop)

    constexpr size_t kComplexF32Size = sizeof(ComplexF32);
    constexpr size_t kRequestHeaderSize = sizeof(RequestHeader);
    constexpr size_t kResponseHeaderSize = sizeof(ResponseHeader);
    constexpr size_t kIqRequestSize = sizeof(IqRequest);
    constexpr size_t kRdmRequestSize = sizeof(RdmRequest);
    constexpr size_t kOptChirpMaskSize = sizeof(OptChirpMask);
    constexpr size_t kOptVaMaskSize = sizeof(OptVaMask);
    constexpr size_t kOptVaListHeaderSize = sizeof(OptVaList);
    constexpr size_t kOptVaListItemSize = sizeof(OptVaList::Item);
    constexpr size_t kIqResponseInfoSize = sizeof(IqResponseInfo);
    constexpr size_t kRdmResponseInfoSize = sizeof(RdmResponseInfo);

    struct IqRequestMsg
    {
        IqRequest request{};
        std::vector<float> distances;
        std::optional<uint64_t> chirp_mask;
        std::optional<std::array<uint16_t, kTileCount>> va_masks;
        std::vector<OptVaList::Item> va_list;
    };

    struct RdmRequestMsg
    {
        RdmRequest request{};
        std::optional<uint64_t> chirp_mask;
        std::optional<std::array<uint16_t, kTileCount>> va_masks;
        std::vector<OptVaList::Item> va_list;
    };

    struct IqResponse
    {
        ResponseHeader header{};
        IqResponseInfo info{};
        std::vector<ComplexF32> payload;
    };

    struct RdmResponse
    {
        ResponseHeader header{};
        RdmResponseInfo info{};
        std::vector<ComplexF32> payload;
    };

    inline size_t chirpSampleCount(const ChirpMode mode)
    {
        switch (mode)
        {
        case ChirpMode::Array:
            return kChirpCount;
        case ChirpMode::Average:
        case ChirpMode::MaxAbs:
            return 1;
        }
        return 0;
    }

    inline size_t chirpPayloadBytes(const ChirpMode mode)
    {
        return chirpSampleCount(mode) * kComplexF32Size;
    }

    inline bool isValidChirpMode(const uint32_t value)
    {
        return value <= static_cast<uint32_t>(ChirpMode::MaxAbs);
    }

    inline bool isValidRequestType(const uint8_t value)
    {
        return value <= static_cast<uint8_t>(RequestType::Rdm);
    }

    inline bool isValidVaSelectMode(const uint16_t value)
    {
        return value <= static_cast<uint16_t>(VaSelectMode::List);
    }

    inline bool isValidVaCombineMode(const uint16_t value)
    {
        return value <= static_cast<uint16_t>(VaCombineMode::Beamform);
    }

    inline size_t iqRequestTailSize(const IqRequest& request, size_t va_list_items = 0)
    {
        size_t bytes = static_cast<size_t>(request.target_count) * sizeof(float);
        if (request.chirp_select_mode == ChirpSelectMode::Mask)
            bytes += kOptChirpMaskSize;
        switch (request.va_select_mode)
        {
        case VaSelectMode::Mask:
            bytes += kOptVaMaskSize;
            break;
        case VaSelectMode::List:
            bytes += kOptVaListHeaderSize + va_list_items * kOptVaListItemSize;
            break;
        default:
            break;
        }
        return bytes;
    }

    inline size_t iqRequestPayloadSize(const IqRequest& request, size_t va_list_items = 0)
    {
        return kIqRequestSize + iqRequestTailSize(request, va_list_items);
    }

    inline size_t rdmRequestPayloadSize(const RdmRequest& request, size_t va_list_items = 0)
    {
        size_t bytes = kRdmRequestSize;
        if (request.chirp_select_mode == ChirpSelectMode::Mask)
            bytes += kOptChirpMaskSize;
        switch (request.va_select_mode)
        {
        case VaSelectMode::Mask:
            bytes += kOptVaMaskSize;
            break;
        case VaSelectMode::List:
            bytes += kOptVaListHeaderSize + va_list_items * kOptVaListItemSize;
            break;
        default:
            break;
        }
        return bytes;
    }

    inline size_t requestBodySize(const IqRequest& request, size_t va_list_items = 0)
    {
        return kRequestHeaderSize + iqRequestPayloadSize(request, va_list_items);
    }

    inline size_t requestBodySize(const RdmRequest& request, size_t va_list_items = 0)
    {
        return kRequestHeaderSize + rdmRequestPayloadSize(request, va_list_items);
    }

    inline std::string validateIqRequest(const IqRequest& request, size_t va_list_items = 0)
    {
        if (request.type != RequestType::Iq)
            return "invalid request type for IqRequest";
        if (!isValidChirpMode(static_cast<uint32_t>(request.chirp_mode)))
            return "invalid chirp_mode";
        if (!isValidVaSelectMode(static_cast<uint16_t>(request.va_select_mode)))
            return "invalid va_select_mode";
        if (!isValidVaCombineMode(static_cast<uint16_t>(request.va_combine_mode)))
            return "invalid va_combine_mode";
        if (request.target_count == 0 || request.target_count > kMaxTargets)
            return "target_count out of range";
        if (request.va_select_mode == VaSelectMode::Single)
        {
            if (request.tile >= kTileCount)
                return "tile out of range";
            if (request.sub_ant >= kSubAntCount)
                return "sub_ant out of range";
        }
        if (request.va_select_mode == VaSelectMode::List)
        {
            if (va_list_items == 0)
                return "va list is empty";
        }
        return {};
    }

    inline std::string validateRdmRequest(const RdmRequest& request, size_t va_list_items = 0)
    {
        if (request.type != RequestType::Rdm)
            return "invalid request type for RdmRequest";
        if (!isValidChirpMode(static_cast<uint32_t>(request.chirp_mode)))
            return "invalid chirp_mode";
        if (!isValidVaSelectMode(static_cast<uint16_t>(request.va_select_mode)))
            return "invalid va_select_mode";
        if (!isValidVaCombineMode(static_cast<uint16_t>(request.va_combine_mode)))
            return "invalid va_combine_mode";
        if (request.va_select_mode == VaSelectMode::Single)
        {
            if (request.tile >= kTileCount)
                return "tile out of range";
            if (request.sub_ant >= kSubAntCount)
                return "sub_ant out of range";
        }
        if (request.distance_max < request.distance_min)
            return "distance_max < distance_min";
        if (request.velocity_max < request.velocity_min)
            return "velocity_max < velocity_min";
        if (request.va_select_mode == VaSelectMode::List && va_list_items == 0)
            return "va list is empty";
        return {};
    }

    inline std::string validateRequestHeader(const RequestHeader& header, size_t payload_size)
    {
        if (header.magic != kRequestMagic)
            return "invalid request magic";
        if (header.version != kProtocolVersion)
            return "unsupported protocol version";
        if (header.payload_size != payload_size)
            return "payload_size mismatch";
        return {};
    }

    inline size_t iqResponsePayloadBytes(const IqResponseInfo& info, ChirpMode mode)
    {
        return static_cast<size_t>(info.target_count) * chirpPayloadBytes(mode);
    }

    inline size_t rdmResponsePayloadBytes(const RdmResponseInfo& info)
    {
        return static_cast<size_t>(info.range_count) * static_cast<size_t>(info.doppler_count) * kComplexF32Size;
    }

    inline size_t iqResponseBodySize(const IqResponseInfo& info, ChirpMode mode)
    {
        return kResponseHeaderSize + kIqResponseInfoSize + iqResponsePayloadBytes(info, mode);
    }

    inline size_t rdmResponseBodySize(const RdmResponseInfo& info)
    {
        return kResponseHeaderSize + kRdmResponseInfoSize + rdmResponsePayloadBytes(info);
    }

    inline bool readIqRequestTail(
        const std::span<const uint8_t>& payload,
        IqRequestMsg& out,
        std::string& error)
    {
        const IqRequest& request = out.request;
        if (payload.size() < kIqRequestSize)
        {
            error = "iq request body too short";
            return false;
        }

        size_t offset = kIqRequestSize;
        const size_t distances_bytes = static_cast<size_t>(request.target_count) * sizeof(float);
        if (payload.size() < offset + distances_bytes)
        {
            error = "iq request distances too short";
            return false;
        }

        out.distances.resize(request.target_count);
        if (request.target_count > 0)
        {
            std::memcpy(out.distances.data(), payload.data() + offset, distances_bytes);
            offset += distances_bytes;
        }

        if (request.chirp_select_mode == ChirpSelectMode::Mask)
        {
            if (payload.size() < offset + kOptChirpMaskSize)
            {
                error = "iq request chirp mask too short";
                return false;
            }
            OptChirpMask mask{};
            std::memcpy(&mask, payload.data() + offset, kOptChirpMaskSize);
            out.chirp_mask = mask.chirp_mask;
            offset += kOptChirpMaskSize;
        }

        size_t va_list_items = 0;
        if (request.va_select_mode == VaSelectMode::Mask)
        {
            if (payload.size() < offset + kOptVaMaskSize)
            {
                error = "iq request va mask too short";
                return false;
            }
            OptVaMask masks{};
            std::memcpy(&masks, payload.data() + offset, kOptVaMaskSize);
            out.va_masks = std::array<uint16_t, kTileCount>{};
            for (size_t i = 0; i < kTileCount; ++i)
                (*out.va_masks)[i] = masks.va_masks[i];
            offset += kOptVaMaskSize;
        }
        else if (request.va_select_mode == VaSelectMode::List)
        {
            if (payload.size() < offset + kOptVaListHeaderSize)
            {
                error = "iq request va list header too short";
                return false;
            }
            OptVaList list_header{};
            std::memcpy(&list_header, payload.data() + offset, kOptVaListHeaderSize);
            offset += kOptVaListHeaderSize;
            va_list_items = list_header.item_count;
            const size_t list_bytes = va_list_items * kOptVaListItemSize;
            if (payload.size() < offset + list_bytes)
            {
                error = "iq request va list too short";
                return false;
            }
            out.va_list.resize(va_list_items);
            if (va_list_items > 0)
            {
                std::memcpy(out.va_list.data(), payload.data() + offset, list_bytes);
                offset += list_bytes;
            }
        }

        if (offset != payload.size())
        {
            error = "iq request payload size mismatch";
            return false;
        }

        error = validateIqRequest(request, va_list_items);
        return error.empty();
    }

    inline bool readRdmRequestTail(
        const std::span<const uint8_t>& payload,
        RdmRequestMsg& out,
        std::string& error)
    {
        const RdmRequest& request = out.request;
        if (payload.size() < kRdmRequestSize)
        {
            error = "rdm request body too short";
            return false;
        }

        size_t offset = kRdmRequestSize;
        if (request.chirp_select_mode == ChirpSelectMode::Mask)
        {
            if (payload.size() < offset + kOptChirpMaskSize)
            {
                error = "rdm request chirp mask too short";
                return false;
            }
            OptChirpMask mask{};
            std::memcpy(&mask, payload.data() + offset, kOptChirpMaskSize);
            out.chirp_mask = mask.chirp_mask;
            offset += kOptChirpMaskSize;
        }

        size_t va_list_items = 0;
        if (request.va_select_mode == VaSelectMode::Mask)
        {
            if (payload.size() < offset + kOptVaMaskSize)
            {
                error = "rdm request va mask too short";
                return false;
            }
            OptVaMask masks{};
            std::memcpy(&masks, payload.data() + offset, kOptVaMaskSize);
            out.va_masks = std::array<uint16_t, kTileCount>{};
            for (size_t i = 0; i < kTileCount; ++i)
                (*out.va_masks)[i] = masks.va_masks[i];
            offset += kOptVaMaskSize;
        }
        else if (request.va_select_mode == VaSelectMode::List)
        {
            if (payload.size() < offset + kOptVaListHeaderSize)
            {
                error = "rdm request va list header too short";
                return false;
            }
            OptVaList list_header{};
            std::memcpy(&list_header, payload.data() + offset, kOptVaListHeaderSize);
            offset += kOptVaListHeaderSize;
            va_list_items = list_header.item_count;
            const size_t list_bytes = va_list_items * kOptVaListItemSize;
            if (payload.size() < offset + list_bytes)
            {
                error = "rdm request va list too short";
                return false;
            }
            out.va_list.resize(va_list_items);
            if (va_list_items > 0)
            {
                std::memcpy(out.va_list.data(), payload.data() + offset, list_bytes);
                offset += list_bytes;
            }
        }

        if (offset != payload.size())
        {
            error = "rdm request payload size mismatch";
            return false;
        }

        error = validateRdmRequest(request, va_list_items);
        return error.empty();
    }

    inline bool readRequest(
        const std::span<const uint8_t>& data,
        RequestType& type,
        IqRequestMsg& iq_out,
        RdmRequestMsg& rdm_out,
        std::string& error)
    {
        if (data.size() < kRequestHeaderSize + 1)
        {
            error = "request too short";
            return false;
        }

        RequestHeader header{};
        std::memcpy(&header, data.data(), kRequestHeaderSize);
        const auto payload = data.subspan(kRequestHeaderSize);
        if (payload.empty())
        {
            error = "empty request payload";
            return false;
        }

        type = static_cast<RequestType>(payload[0]);
        if (!isValidRequestType(static_cast<uint8_t>(type)))
        {
            error = "invalid request_type";
            return false;
        }

        if (type == RequestType::Iq)
        {
            if (payload.size() < kIqRequestSize)
            {
                error = "iq request body too short";
                return false;
            }
            std::memcpy(&iq_out.request, payload.data(), kIqRequestSize);
            error = validateRequestHeader(header, payload.size());
            if (!error.empty())
                return false;
            return readIqRequestTail(payload, iq_out, error);
        }

        if (payload.size() < kRdmRequestSize)
        {
            error = "rdm request body too short";
            return false;
        }
        std::memcpy(&rdm_out.request, payload.data(), kRdmRequestSize);
        error = validateRequestHeader(header, payload.size());
        if (!error.empty())
            return false;
        return readRdmRequestTail(payload, rdm_out, error);
    }

    inline std::vector<uint8_t> writeIqRequest(const IqRequestMsg& message)
    {
        const size_t payload_size = iqRequestPayloadSize(message.request, message.va_list.size());
        std::vector<uint8_t> buffer(kRequestHeaderSize + payload_size);
        RequestHeader header{
            kRequestMagic,
            kProtocolVersion,
            static_cast<uint32_t>(payload_size),
        };
        std::memcpy(buffer.data(), &header, kRequestHeaderSize);

        size_t offset = kRequestHeaderSize;
        std::memcpy(buffer.data() + offset, &message.request, kIqRequestSize);
        offset += kIqRequestSize;

        if (!message.distances.empty())
        {
            std::memcpy(
                buffer.data() + offset,
                message.distances.data(),
                message.distances.size() * sizeof(float));
            offset += message.distances.size() * sizeof(float);
        }

        if (message.request.chirp_select_mode == ChirpSelectMode::Mask && message.chirp_mask.has_value())
        {
            OptChirpMask mask{*message.chirp_mask};
            std::memcpy(buffer.data() + offset, &mask, kOptChirpMaskSize);
            offset += kOptChirpMaskSize;
        }

        if (message.request.va_select_mode == VaSelectMode::Mask && message.va_masks.has_value())
        {
            OptVaMask masks{};
            for (size_t i = 0; i < kTileCount; ++i)
                masks.va_masks[i] = (*message.va_masks)[i];
            std::memcpy(buffer.data() + offset, &masks, kOptVaMaskSize);
            offset += kOptVaMaskSize;
        }
        else if (message.request.va_select_mode == VaSelectMode::List)
        {
            OptVaList list_header{static_cast<uint16_t>(message.va_list.size())};
            std::memcpy(buffer.data() + offset, &list_header, kOptVaListHeaderSize);
            offset += kOptVaListHeaderSize;
            if (!message.va_list.empty())
            {
                std::memcpy(buffer.data() + offset, message.va_list.data(), message.va_list.size() * kOptVaListItemSize);
                offset += message.va_list.size() * kOptVaListItemSize;
            }
        }

        return buffer;
    }

    inline std::vector<uint8_t> writeRdmRequest(const RdmRequestMsg& message)
    {
        const size_t payload_size = rdmRequestPayloadSize(message.request, message.va_list.size());
        std::vector<uint8_t> buffer(kRequestHeaderSize + payload_size);
        RequestHeader header{
            kRequestMagic,
            kProtocolVersion,
            static_cast<uint32_t>(payload_size),
        };
        std::memcpy(buffer.data(), &header, kRequestHeaderSize);

        size_t offset = kRequestHeaderSize;
        std::memcpy(buffer.data() + offset, &message.request, kRdmRequestSize);
        offset += kRdmRequestSize;

        if (message.request.chirp_select_mode == ChirpSelectMode::Mask && message.chirp_mask.has_value())
        {
            OptChirpMask mask{*message.chirp_mask};
            std::memcpy(buffer.data() + offset, &mask, kOptChirpMaskSize);
            offset += kOptChirpMaskSize;
        }

        if (message.request.va_select_mode == VaSelectMode::Mask && message.va_masks.has_value())
        {
            OptVaMask masks{};
            for (size_t i = 0; i < kTileCount; ++i)
                masks.va_masks[i] = (*message.va_masks)[i];
            std::memcpy(buffer.data() + offset, &masks, kOptVaMaskSize);
            offset += kOptVaMaskSize;
        }
        else if (message.request.va_select_mode == VaSelectMode::List)
        {
            OptVaList list_header{static_cast<uint16_t>(message.va_list.size())};
            std::memcpy(buffer.data() + offset, &list_header, kOptVaListHeaderSize);
            offset += kOptVaListHeaderSize;
            if (!message.va_list.empty())
            {
                std::memcpy(buffer.data() + offset, message.va_list.data(), message.va_list.size() * kOptVaListItemSize);
                offset += message.va_list.size() * kOptVaListItemSize;
            }
        }

        return buffer;
    }

    inline bool readIqResponse(
        const std::span<const uint8_t>& data,
        ChirpMode mode,
        IqResponse& out,
        std::string& error)
    {
        if (data.size() < kResponseHeaderSize + kIqResponseInfoSize)
        {
            error = "iq response too short";
            return false;
        }

        std::memcpy(&out.header, data.data(), kResponseHeaderSize);
        if (out.header.magic != kResponseMagic)
        {
            error = "invalid response magic";
            return false;
        }
        if (out.header.version != kProtocolVersion)
        {
            error = "unsupported response version";
            return false;
        }

        std::memcpy(&out.info, data.data() + kResponseHeaderSize, kIqResponseInfoSize);
        const size_t expected = iqResponseBodySize(out.info, mode);
        if (data.size() < expected)
        {
            error = "iq response payload too short";
            return false;
        }

        const size_t sample_count = iqResponsePayloadBytes(out.info, mode) / kComplexF32Size;
        out.payload.resize(sample_count);
        if (sample_count > 0)
        {
            std::memcpy(
                out.payload.data(),
                data.data() + kResponseHeaderSize + kIqResponseInfoSize,
                sample_count * kComplexF32Size);
        }
        return true;
    }

    inline bool readRdmResponse(const std::span<const uint8_t>& data, RdmResponse& out, std::string& error)
    {
        if (data.size() < kResponseHeaderSize + kRdmResponseInfoSize)
        {
            error = "rdm response too short";
            return false;
        }

        std::memcpy(&out.header, data.data(), kResponseHeaderSize);
        if (out.header.magic != kResponseMagic)
        {
            error = "invalid response magic";
            return false;
        }
        if (out.header.version != kProtocolVersion)
        {
            error = "unsupported response version";
            return false;
        }

        std::memcpy(&out.info, data.data() + kResponseHeaderSize, kRdmResponseInfoSize);
        const size_t expected = rdmResponseBodySize(out.info);
        if (data.size() < expected)
        {
            error = "rdm response payload too short";
            return false;
        }

        const size_t cell_count = rdmResponsePayloadBytes(out.info) / kComplexF32Size;
        out.payload.resize(cell_count);
        if (cell_count > 0)
        {
            std::memcpy(
                out.payload.data(),
                data.data() + kResponseHeaderSize + kRdmResponseInfoSize,
                cell_count * kComplexF32Size);
        }
        return true;
    }

    inline std::vector<uint8_t> writeIqResponse(const IqResponse& response)
    {
        const size_t body_size = kResponseHeaderSize + kIqResponseInfoSize + response.payload.size() * kComplexF32Size;
        std::vector<uint8_t> buffer(body_size);
        ResponseHeader header = response.header;
        header.magic = kResponseMagic;
        header.version = kProtocolVersion;
        header.payload_size = static_cast<uint32_t>(body_size - kResponseHeaderSize);
        std::memcpy(buffer.data(), &header, kResponseHeaderSize);
        std::memcpy(buffer.data() + kResponseHeaderSize, &response.info, kIqResponseInfoSize);
        if (!response.payload.empty())
        {
            std::memcpy(
                buffer.data() + kResponseHeaderSize + kIqResponseInfoSize,
                response.payload.data(),
                response.payload.size() * kComplexF32Size);
        }
        return buffer;
    }

    inline std::vector<uint8_t> writeRdmResponse(const RdmResponse& response)
    {
        const size_t body_size = kResponseHeaderSize + kRdmResponseInfoSize + response.payload.size() * kComplexF32Size;
        std::vector<uint8_t> buffer(body_size);
        ResponseHeader header = response.header;
        header.magic = kResponseMagic;
        header.version = kProtocolVersion;
        header.payload_size = static_cast<uint32_t>(body_size - kResponseHeaderSize);
        std::memcpy(buffer.data(), &header, kResponseHeaderSize);
        std::memcpy(buffer.data() + kResponseHeaderSize, &response.info, kRdmResponseInfoSize);
        if (!response.payload.empty())
        {
            std::memcpy(
                buffer.data() + kResponseHeaderSize + kRdmResponseInfoSize,
                response.payload.data(),
                response.payload.size() * kComplexF32Size);
        }
        return buffer;
    }

}  // namespace iq
