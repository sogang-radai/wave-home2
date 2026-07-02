#include "mic_capture.h"

#if defined(__APPLE__)

#include <AudioToolbox/AudioToolbox.h>

#include <cstring>
#include <iostream>
#include <vector>

namespace
{
    struct CaptureState
    {
        MicCapture::ChunkCallback callback;
        std::vector<float> convertBuffer;
        int32_t channels = 1;
    };

    void audioQueueCallback(
        void* userData,
        AudioQueueRef queue,
        AudioQueueBufferRef buffer,
        const AudioTimeStamp* /*timeStamp*/,
        UInt32 /*numPackets*/,
        const AudioStreamPacketDescription* /*packetDesc*/)
    {
        auto* state = static_cast<CaptureState*>(userData);
        if (state == nullptr || !state->callback || buffer == nullptr)
            return;

        const auto* samples = reinterpret_cast<const float*>(buffer->mAudioData);
        const size_t frame_count =
            buffer->mAudioDataByteSize / sizeof(float) / static_cast<size_t>(state->channels);
        const size_t sample_count = frame_count * static_cast<size_t>(state->channels);
        state->callback(samples, sample_count);

        AudioQueueEnqueueBuffer(queue, buffer, 0, nullptr);
    }

    int32_t countInputChannels(AudioDeviceID device_id)
    {
        AudioObjectPropertyAddress config_address{
            kAudioDevicePropertyStreamConfiguration,
            kAudioDevicePropertyScopeInput,
            kAudioObjectPropertyElementMain,
        };

        UInt32 data_size = 0;
        OSStatus status = AudioObjectGetPropertyDataSize(
            device_id,
            &config_address,
            0,
            nullptr,
            &data_size);
        if (status != noErr || data_size == 0)
            return 0;

        std::vector<uint8_t> buffer(data_size);
        status = AudioObjectGetPropertyData(
            device_id,
            &config_address,
            0,
            nullptr,
            &data_size,
            buffer.data());
        if (status != noErr)
            return 0;

        const auto* list = reinterpret_cast<const AudioBufferList*>(buffer.data());
        int32_t channels = 0;
        for (UInt32 i = 0; i < list->mNumberBuffers; ++i)
            channels += static_cast<int32_t>(list->mBuffers[i].mNumberChannels);
        return channels;
    }
}

struct MicCapture::Impl
{
    AudioQueueRef queue = nullptr;
    CaptureState state;
    std::vector<AudioQueueBufferRef> buffers;
    bool open = false;
    int32_t deviceIndex = -1;
    int32_t sampleRate = 0;
    int32_t channels = 0;
};

MicCapture::MicCapture() :
    m_impl(std::make_unique<Impl>())
{
}

MicCapture::~MicCapture()
{
    close();
}

std::vector<MicDeviceInfo> MicCapture::listInputDevices()
{
    std::vector<MicDeviceInfo> devices;

    AudioObjectPropertyAddress devices_address{
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain,
    };

    UInt32 property_size = 0;
    OSStatus status = AudioObjectGetPropertyDataSize(
        kAudioObjectSystemObject,
        &devices_address,
        0,
        nullptr,
        &property_size);
    if (status != noErr || property_size == 0)
        return devices;

    const UInt32 device_count = property_size / sizeof(AudioDeviceID);
    std::vector<AudioDeviceID> device_ids(device_count);
    status = AudioObjectGetPropertyData(
        kAudioObjectSystemObject,
        &devices_address,
        0,
        nullptr,
        &property_size,
        device_ids.data());
    if (status != noErr)
        return devices;

    AudioDeviceID default_input = kAudioObjectUnknown;
    UInt32 default_size = sizeof(default_input);
    AudioObjectPropertyAddress default_address{
        kAudioHardwarePropertyDefaultInputDevice,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain,
    };
    AudioObjectGetPropertyData(
        kAudioObjectSystemObject,
        &default_address,
        0,
        nullptr,
        &default_size,
        &default_input);

    int32_t index = 0;
    for (AudioDeviceID device_id : device_ids)
    {
        const int32_t input_channels = countInputChannels(device_id);
        if (input_channels == 0)
            continue;

        CFStringRef device_name = nullptr;
        UInt32 name_size = sizeof(device_name);
        AudioObjectPropertyAddress name_address{
            kAudioObjectPropertyName,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain,
        };
        status = AudioObjectGetPropertyData(
            device_id,
            &name_address,
            0,
            nullptr,
            &name_size,
            &device_name);
        if (status != noErr)
            continue;

        Float64 nominal_rate = 0.0;
        UInt32 rate_size = sizeof(nominal_rate);
        AudioObjectPropertyAddress rate_address{
            kAudioDevicePropertyNominalSampleRate,
            kAudioDevicePropertyScopeInput,
            kAudioObjectPropertyElementMain,
        };
        AudioObjectGetPropertyData(
            device_id,
            &rate_address,
            0,
            nullptr,
            &rate_size,
            &nominal_rate);

        MicDeviceInfo device;
        device.index = index++;
        device.deviceId = device_id;
        device.maxInputChannels = input_channels;
        device.defaultSampleRate = nominal_rate;
        device.isDefault = device_id == default_input;

        char name_buffer[256] = {};
        if (CFStringGetCString(device_name, name_buffer, sizeof(name_buffer), kCFStringEncodingUTF8))
            device.name = name_buffer;
        else
            device.name = "Unknown device";

        CFRelease(device_name);
        devices.push_back(std::move(device));
    }

    return devices;
}

int32_t MicCapture::defaultInputDeviceIndex()
{
    const std::vector<MicDeviceInfo> devices = listInputDevices();
    for (const MicDeviceInfo& device : devices)
    {
        if (device.isDefault)
            return device.index;
    }
    return devices.empty() ? -1 : devices.front().index;
}

bool MicCapture::open(
    int32_t deviceIndex,
    int32_t sampleRate,
    int32_t channels,
    ChunkCallback callback,
    bool useExplicitDevice)
{
    close();

    if (!callback)
    {
        std::cerr << "MicCapture callback is required.\n";
        return false;
    }

    const std::vector<MicDeviceInfo> devices = listInputDevices();
    if (devices.empty())
    {
        std::cerr << "No input devices found.\n";
        return false;
    }

    if (deviceIndex < 0)
        deviceIndex = defaultInputDeviceIndex();

    const MicDeviceInfo* selected = nullptr;
    for (const MicDeviceInfo& device : devices)
    {
        if (device.index == deviceIndex)
        {
            selected = &device;
            break;
        }
    }

    if (selected == nullptr)
    {
        std::cerr << "Invalid input device index: " << deviceIndex << "\n";
        return false;
    }

    if (sampleRate <= 0 || channels <= 0)
    {
        std::cerr << "Invalid sample rate or channel count.\n";
        return false;
    }

    if (channels > selected->maxInputChannels)
    {
        std::cerr << "Device supports at most " << selected->maxInputChannels
                  << " input channels; requested " << channels << ".\n";
        return false;
    }

    AudioStreamBasicDescription format{};
    format.mSampleRate = static_cast<Float64>(sampleRate);
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    format.mBitsPerChannel = 32;
    format.mChannelsPerFrame = static_cast<UInt32>(channels);
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = static_cast<UInt32>(channels * sizeof(float));
    format.mBytesPerPacket = format.mBytesPerFrame;

    OSStatus status = AudioQueueNewInput(
        &format,
        audioQueueCallback,
        &m_impl->state,
        nullptr,
        nullptr,
        0,
        &m_impl->queue);
    if (status != noErr || m_impl->queue == nullptr)
    {
        std::cerr << "AudioQueueNewInput failed: " << status << "\n";
        return false;
    }

    if (useExplicitDevice && !selected->isDefault)
    {
        UInt32 device_id = selected->deviceId;
        status = AudioQueueSetProperty(
            m_impl->queue,
            kAudioQueueProperty_CurrentDevice,
            &device_id,
            sizeof(device_id));
        if (status != noErr)
        {
            std::cerr << "AudioQueueSetProperty(CurrentDevice) failed: " << status << "\n";
            close();
            return false;
        }
    }

    constexpr UInt32 kBufferCount = 3;
    constexpr UInt32 kFramesPerBuffer = 1024;
    const UInt32 buffer_byte_size = kFramesPerBuffer * format.mBytesPerFrame;
    m_impl->buffers.resize(kBufferCount);

    for (UInt32 i = 0; i < kBufferCount; ++i)
    {
        status = AudioQueueAllocateBuffer(m_impl->queue, buffer_byte_size, &m_impl->buffers[i]);
        if (status != noErr)
        {
            std::cerr << "AudioQueueAllocateBuffer failed: " << status << "\n";
            close();
            return false;
        }

        std::memset(m_impl->buffers[i]->mAudioData, 0, m_impl->buffers[i]->mAudioDataBytesCapacity);
        m_impl->buffers[i]->mAudioDataByteSize = buffer_byte_size;
        status = AudioQueueEnqueueBuffer(m_impl->queue, m_impl->buffers[i], 0, nullptr);
        if (status != noErr)
        {
            std::cerr << "AudioQueueEnqueueBuffer failed: " << status << "\n";
            close();
            return false;
        }
    }

    m_impl->state.callback = std::move(callback);
    m_impl->state.channels = channels;

    status = AudioQueueStart(m_impl->queue, nullptr);
    if (status != noErr)
    {
        std::cerr << "AudioQueueStart failed: " << status << "\n";
        close();
        return false;
    }

    m_impl->open = true;
    m_impl->deviceIndex = deviceIndex;
    m_impl->sampleRate = sampleRate;
    m_impl->channels = channels;

    std::cout << "Microphone opened: [" << deviceIndex << "] " << selected->name
              << " @ " << sampleRate << " Hz, " << channels << " ch\n";
    return true;
}

void MicCapture::close()
{
    if (m_impl->queue == nullptr)
        return;

    AudioQueueStop(m_impl->queue, true);
    for (AudioQueueBufferRef buffer : m_impl->buffers)
    {
        if (buffer != nullptr)
            AudioQueueFreeBuffer(m_impl->queue, buffer);
    }
    m_impl->buffers.clear();

    AudioQueueDispose(m_impl->queue, true);
    m_impl->queue = nullptr;
    m_impl->state.callback = {};
    m_impl->open = false;
    m_impl->deviceIndex = -1;
    m_impl->sampleRate = 0;
    m_impl->channels = 0;
}

bool MicCapture::isOpen() const
{
    return m_impl->open;
}

int32_t MicCapture::deviceIndex() const
{
    return m_impl->deviceIndex;
}

int32_t MicCapture::sampleRate() const
{
    return m_impl->sampleRate;
}

int32_t MicCapture::channels() const
{
    return m_impl->channels;
}

#else

#include <iostream>

struct MicCapture::Impl
{
};

MicCapture::MicCapture() :
    m_impl(std::make_unique<Impl>())
{
}

MicCapture::~MicCapture()
{
    close();
}

std::vector<MicDeviceInfo> MicCapture::listInputDevices()
{
    return {};
}

int32_t MicCapture::defaultInputDeviceIndex()
{
    return -1;
}

bool MicCapture::open(
    int32_t /*deviceIndex*/,
    int32_t /*sampleRate*/,
    int32_t /*channels*/,
    ChunkCallback /*callback*/,
    bool /*useExplicitDevice*/)
{
    std::cerr << "Microphone capture is only supported on macOS.\n";
    return false;
}

void MicCapture::close()
{
}

bool MicCapture::isOpen() const
{
    return false;
}

int32_t MicCapture::deviceIndex() const
{
    return -1;
}

int32_t MicCapture::sampleRate() const
{
    return 0;
}

int32_t MicCapture::channels() const
{
    return 0;
}

#endif
