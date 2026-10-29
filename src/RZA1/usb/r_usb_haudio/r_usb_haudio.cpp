/*
 * Copyright © 2026 Synthstrom Audible Limited
 *
 * Host-side USB Audio capture for the Deluge. The Renesas stack calls the class callbacks from its cooperative USB
 * task. Full-speed packets are converted there; high-speed packets are drained in the USB ISR. Both feed the
 * lock-free input ring consumed by the audio engine.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "RZA1/usb/r_usb_haudio/r_usb_haudio_if.h"
#include "RZA1/usb/r_usb_haudio/r_usb_haudio_transport.h"

#include "RZA1/usb/r_usb_basic/r_usb_basic_if.h"
#include "RZA1/usb/r_usb_basic/src/driver/inc/r_usb_basic_define.h"
extern "C" {
#include "RZA1/usb/r_usb_basic/src/driver/inc/r_usb_extern.h"
extern uint16_t g_usb_hstd_use_pipe[USB_NUM_USBIP];
}
#include "RZA1/usb/userdef/r_usb_basic_config.h"
#include "definitions.h"
#include "deluge/drivers/usb/hosted_audio.h"
#include "deluge/drivers/usb/usb_audio.h"
#include "deluge/drivers/usb/usb_mode.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#if ENABLE_TEXT_OUTPUT
#include "RZA1/mtu/mtu.h"
#endif

namespace {

constexpr uint16_t kAudioPipe         = USB_PIPE2;
constexpr uint16_t kAudioBufferNumber = 48; // 48..79 for two 1024-byte planes; MIDI/hub use 0..47.
constexpr size_t kMaxPacketBytes      = 1024;
constexpr size_t kDecodeChunkFrames   = 32;

std::array<uint16_t, 2 + (USB_CFG_TPLCNT * 2)> target_peripheral_list = {
    USB_CFG_TPLCNT,
    0,
    USB_CFG_TPL,
};

std::array<uint16_t, USB_EPL + 1> pipe_table = {
    kAudioPipe,
    USB_NULL,
    static_cast<uint16_t>(USB_BUF_SIZE(1024u) | USB_BUF_NUMB(kAudioBufferNumber)),
    USB_NULL,
    USB_NULL,
    USB_NULL,
    USB_PDTBLEND,
};

std::optional<deluge::hosted_audio::InputStream> selected_stream;
uint16_t device_address = USB_NODEVICE;
uint16_t device_speed   = USB_NOCONNECT;
bool streaming          = false;
std::atomic<bool> irq_streaming{false};
bool irq_transport_owned = false; // USB task only; ensures teardown runs even after a receive error.
usb_utr_t audio_context{};
std::atomic<usb_host_audio_status_t> audio_status{USB_HOST_AUDIO_IDLE};
std::atomic<uint32_t> received_packets{0};
#if ENABLE_TEXT_OUTPUT
std::atomic<uint32_t> max_irq_ticks{0};
#endif
static_assert(std::atomic<bool>::is_always_lock_free && std::atomic<uint32_t>::is_always_lock_free);

alignas(4) std::array<uint8_t, kMaxPacketBytes + 1> packet_buffer;
PLACE_SDRAM_BSS std::array<int32_t, kDecodeChunkFrames * 2> decoded_samples;
usb_utr_t stream_transfer{};
usb_utr_t control_transfer{};
std::array<uint16_t, 5> control_setup{};
std::array<uint8_t, 4> sample_rate_data{};
uint32_t requested_bytes = 0;

uint16_t configurationLength(const uint8_t* configuration)
{
    return static_cast<uint16_t>(configuration[2]) | static_cast<uint16_t>(configuration[3]) << 8;
}

uint8_t* findEndpoint(uint8_t* configuration, uint16_t length, const deluge::hosted_audio::InputStream& stream)
{
    bool in_selected_alternate = false;
    for (uint16_t offset = 0; offset + 2 <= length;)
    {
        uint8_t descriptor_length = configuration[offset];
        if (descriptor_length < 2 || offset + descriptor_length > length)
        {
            return nullptr;
        }
        uint8_t* descriptor = configuration + offset;
        if (descriptor[1] == USB_DT_INTERFACE && descriptor_length >= 9)
        {
            in_selected_alternate = descriptor[2] == stream.interfaceNumber && descriptor[3] == stream.alternateSetting;
        }
        else if (in_selected_alternate && descriptor[1] == USB_DT_ENDPOINT && descriptor_length >= 7
                 && descriptor[2] == stream.endpointAddress)
        {
            return descriptor;
        }
        offset += descriptor_length;
    }
    return nullptr;
}

void queuePacket();
void selectStreamingInterface(usb_utr_t* ptr);

void publishPacket(size_t received)
{
    if (!selected_stream)
    {
        return;
    }
    size_t frame_bytes = selected_stream->channels * selected_stream->subslotBytes;
    if (received == 0 || received % frame_bytes != 0)
    {
        return;
    }
    for (size_t offset = 0; offset < received;)
    {
        size_t bytes = std::min(received - offset, kDecodeChunkFrames * frame_bytes);
        size_t frames =
            deluge::hosted_audio::decodePcm({packet_buffer.data() + offset, bytes}, *selected_stream, decoded_samples);
        deluge::usb_audio::receiveHostedInput({decoded_samples.data(), frames * 2});
        offset += bytes;
    }
    received_packets.fetch_add(1, std::memory_order_relaxed);
    audio_status = USB_HOST_AUDIO_RECEIVING;
}

void packetComplete(usb_utr_t* transfer, uint16_t /*unused*/, uint16_t /*unused*/)
{
    if (!streaming || !selected_stream)
    {
        return;
    }

    if (transfer->status != USB_DATA_READING)
    {
        // In particular, detach/forced termination must not submit another transfer to a disappearing device.
        streaming    = false;
        audio_status = USB_HOST_AUDIO_TRANSFER_ERROR;
        deluge::usb_audio::stopHostedInput();
        return;
    }

    uint32_t remaining = std::min<uint32_t>(transfer->tranlen, requested_bytes);
    uint32_t received  = requested_bytes - remaining;
    publishPacket(received);

    queuePacket();
}

void queuePacket()
{
    if (!streaming || !selected_stream)
    {
        return;
    }

    requested_bytes           = std::min<uint32_t>(selected_stream->maxPacketBytes, packet_buffer.size());
    stream_transfer.keyword   = kAudioPipe;
    stream_transfer.p_tranadr = packet_buffer.data();
    stream_transfer.tranlen   = requested_bytes;
    stream_transfer.complete  = packetComplete;
    stream_transfer.segment   = USB_TRAN_END;
    stream_transfer.ip        = USB_CFG_USE_USBIP;
    stream_transfer.ipp       = usb_hstd_get_usb_ip_adr(USB_CFG_USE_USBIP);

    if (usb_hstd_transfer_start(&stream_transfer) != USB_OK)
    {
        streaming    = false;
        audio_status = USB_HOST_AUDIO_TRANSFER_ERROR;
        deluge::usb_audio::stopHostedInput();
    }
}

void startStreaming()
{
    if (!selected_stream || device_address == USB_NODEVICE)
    {
        return;
    }
    streaming    = true;
    audio_status = USB_HOST_AUDIO_WAITING_PACKETS;
    deluge::usb_audio::startHostedInput();
    if (device_speed == USB_HSCONNECT)
    {
        // Publish the fully configured stream before enabling its interrupt. Reception must not wait for the
        // 0.5ms USB task: TONEX produces a new packet every 125us. D1FIFO is independent of control/MIDI CFIFO.
        irq_transport_owned = true;
        irq_streaming.store(true, std::memory_order_release);
        if (usb_hstd_audio_transport_start(&audio_context) != USB_OK)
        {
            irq_streaming.store(false, std::memory_order_release);
            streaming    = false;
            audio_status = USB_HOST_AUDIO_TRANSFER_ERROR;
            deluge::usb_audio::stopHostedInput();
        }
    }
    else
    {
        queuePacket();
    }
}

void readSampleRate(usb_utr_t* ptr);

void sampleRateReadComplete(usb_utr_t* transfer, uint16_t /*unused*/, uint16_t /*unused*/)
{
    if (!selected_stream || device_address == USB_NODEVICE)
    {
        return;
    }
    uint32_t actual_rate = static_cast<uint32_t>(sample_rate_data[0]) | static_cast<uint32_t>(sample_rate_data[1]) << 8
                           | static_cast<uint32_t>(sample_rate_data[2]) << 16
                           | static_cast<uint32_t>(sample_rate_data[3]) << 24;
    if (transfer->status != USB_CTRL_END || transfer->tranlen != 0 || actual_rate != selected_stream->sampleRate)
    {
        audio_status = USB_HOST_AUDIO_CLOCK_ERROR;
        return;
    }
    if (selected_stream->version == deluge::hosted_audio::AudioClassVersion::UAC2)
    {
        selectStreamingInterface(transfer);
    }
    else
    {
        startStreaming();
    }
}

void sampleRateComplete(usb_utr_t* transfer, uint16_t /*unused*/, uint16_t /*unused*/)
{
    if (!selected_stream || device_address == USB_NODEVICE)
    {
        return;
    }
    if (transfer->status != USB_CTRL_END)
    {
        audio_status = USB_HOST_AUDIO_CLOCK_ERROR;
        return;
    }
    readSampleRate(transfer);
}

void readSampleRate(usb_utr_t* ptr)
{
    if (!selected_stream)
    {
        return;
    }
    bool uac2 = selected_stream->version == deluge::hosted_audio::AudioClassVersion::UAC2;
    sample_rate_data.fill(0);
    control_setup[0] = (uac2 ? 0x0100u : 0x8100u) | USB_DEV_TO_HOST | USB_CLASS | (uac2 ? USB_INTERFACE : USB_ENDPOINT);
    control_setup[1] = 0x0100u;
    control_setup[2] =
        uac2 ? static_cast<uint16_t>(selected_stream->clockSourceId << 8 | selected_stream->controlInterfaceNumber)
             : selected_stream->endpointAddress;
    control_setup[3]           = uac2 ? 4 : 3;
    control_setup[4]           = device_address;
    control_transfer.p_tranadr = sample_rate_data.data();
    control_transfer.complete  = sampleRateReadComplete;
    control_transfer.tranlen   = control_setup[3];
    control_transfer.keyword   = USB_PIPE0;
    control_transfer.p_setup   = control_setup.data();
    control_transfer.segment   = USB_TRAN_END;
    control_transfer.ip        = ptr->ip;
    control_transfer.ipp       = ptr->ipp;
    if (usb_hstd_transfer_start(&control_transfer) != USB_OK)
    {
        audio_status = USB_HOST_AUDIO_CLOCK_ERROR;
    }
}

void setSampleRate(usb_utr_t* ptr)
{
    if (!selected_stream)
    {
        return;
    }
    uint32_t rate       = selected_stream->sampleRate;
    sample_rate_data[0] = rate;
    sample_rate_data[1] = rate >> 8;
    sample_rate_data[2] = rate >> 16;
    sample_rate_data[3] = rate >> 24;

    control_setup[0] =
        0x0100u | USB_HOST_TO_DEV | USB_CLASS
        | (selected_stream->version == deluge::hosted_audio::AudioClassVersion::UAC2 ? USB_INTERFACE : USB_ENDPOINT);
    control_setup[1] = 0x0100u; // SAM_FREQ_CONTROL
    control_setup[2] =
        selected_stream->version == deluge::hosted_audio::AudioClassVersion::UAC2
            ? static_cast<uint16_t>(selected_stream->clockSourceId << 8 | selected_stream->controlInterfaceNumber)
            : selected_stream->endpointAddress;
    control_setup[3] = selected_stream->version == deluge::hosted_audio::AudioClassVersion::UAC2 ? 4 : 3;
    control_setup[4] = device_address;

    control_transfer.p_tranadr = sample_rate_data.data();
    control_transfer.complete  = sampleRateComplete;
    control_transfer.tranlen   = control_setup[3];
    control_transfer.keyword   = USB_PIPE0;
    control_transfer.p_setup   = control_setup.data();
    control_transfer.segment   = USB_TRAN_END;
    control_transfer.ip        = ptr->ip;
    control_transfer.ipp       = ptr->ipp;
    if (usb_hstd_transfer_start(&control_transfer) != USB_OK)
    {
        audio_status = USB_HOST_AUDIO_CLOCK_ERROR;
    }
}

void setInterfaceComplete(usb_utr_t* transfer, uint16_t /*unused*/, uint16_t /*unused*/)
{
    if (!selected_stream || device_address == USB_NODEVICE)
    {
        return;
    }
    if (transfer->status != USB_CTRL_END)
    {
        audio_status = USB_HOST_AUDIO_INTERFACE_ERROR;
        return;
    }
    if (selected_stream->version == deluge::hosted_audio::AudioClassVersion::UAC1
        && selected_stream->sampleRateNeedsControlRequest)
    {
        setSampleRate(transfer);
        return;
    }
    startStreaming();
}

void selectStreamingInterface(usb_utr_t* ptr)
{
    if (!selected_stream)
    {
        return;
    }
    control_setup[0] = USB_SET_INTERFACE | USB_HOST_TO_DEV | USB_STANDARD | USB_INTERFACE;
    control_setup[1] = selected_stream->alternateSetting;
    control_setup[2] = selected_stream->interfaceNumber;
    control_setup[3] = 0;
    control_setup[4] = device_address;

    control_transfer.p_tranadr = nullptr;
    control_transfer.complete  = setInterfaceComplete;
    control_transfer.tranlen   = 0;
    control_transfer.keyword   = USB_PIPE0;
    control_transfer.p_setup   = control_setup.data();
    control_transfer.segment   = USB_TRAN_END;
    control_transfer.ip        = ptr->ip;
    control_transfer.ipp       = ptr->ipp;
    if (usb_hstd_transfer_start(&control_transfer) != USB_OK)
    {
        audio_status = USB_HOST_AUDIO_INTERFACE_ERROR;
    }
}

void classCheck(usb_utr_t* ptr, uint16_t** table)
{
    *table[3] = USB_ERROR;
    if (device_address != USB_NODEVICE)
    {
        return; // Do not overwrite the status of an already selected device.
    }
    audio_status     = USB_HOST_AUDIO_UNSUPPORTED;
    received_packets = 0;
    if (*table[6] != USB_FSCONNECT && *table[6] != USB_HSCONNECT)
    {
        return;
    }
    if ((g_usb_hstd_use_pipe[ptr->ip] & USB_BITSET(kAudioPipe)) != 0)
    {
        audio_status = USB_HOST_AUDIO_PIPE_IN_USE;
        return;
    }
    // Renesas passes descriptor bytes as uint16_t*.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto* configuration = reinterpret_cast<uint8_t*>(table[1]);
    if (configurationLength(configuration) > USB_CONFIGSIZE)
    {
        audio_status = USB_HOST_AUDIO_CONFIG_TOO_LARGE;
        return;
    }
    // Renesas passes descriptor bytes as uint16_t*.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto* control_interface = reinterpret_cast<uint8_t*>(table[2]);
    uint16_t length         = std::min<uint16_t>(configurationLength(configuration), USB_CONFIGSIZE);
    selected_stream         = deluge::hosted_audio::findInputStream({configuration, length},
                deluge::hosted_audio::kDelugeSampleRate, control_interface[2],
        *table[6] == USB_HSCONNECT ? deluge::hosted_audio::BusSpeed::HIGH : deluge::hosted_audio::BusSpeed::FULL);
    if (!selected_stream || selected_stream->maxPacketBytes > kMaxPacketBytes)
    {
        *table[3] = USB_ERROR;
        return;
    }

    auto* const endpoint = findEndpoint(configuration, length, *selected_stream);
    if (endpoint == nullptr)
    {
        *table[3] = USB_ERROR;
        return;
    }

    device_speed  = *table[6];
    pipe_table[1] = device_speed == USB_HSCONNECT ? USB_DBLBFIELD : USB_NULL;
    pipe_table[2] = static_cast<uint16_t>(USB_BUF_SIZE(1024u) | USB_BUF_NUMB(kAudioBufferNumber));
    pipe_table[3] = USB_NULL;
    pipe_table[4] = USB_NULL;
    if (usb_hstd_chk_pipe_info(device_speed, pipe_table.data(), endpoint) == USB_ERROR)
    {
        selected_stream.reset();
        *table[3] = USB_ERROR;
        return;
    }

    device_address = *table[7];
    audio_status   = USB_HOST_AUDIO_CONFIGURING;
    *table[3]      = USB_OK;
    usb_hstd_return_enu_mgr(ptr, USB_OK);
}

void configured(usb_utr_t* ptr, uint16_t devaddr, uint16_t /*unused*/)
{
    if (!selected_stream || devaddr != device_address)
    {
        return;
    }
    audio_context.ip  = ptr->ip;
    audio_context.ipp = ptr->ipp;
    pipe_table[3] |= static_cast<uint16_t>(devaddr << USB_DEVADDRBIT);
    usb_hstd_set_pipe_registration(ptr, pipe_table.data(), USB_USEPIPE);
    if (selected_stream->version == deluge::hosted_audio::AudioClassVersion::UAC2)
    {
        if (selected_stream->clockFrequencyControls == 3)
        {
            setSampleRate(ptr);
        }
        else
        {
            readSampleRate(ptr);
        }
    }
    else
    {
        selectStreamingInterface(ptr);
    }
}

void detached(usb_utr_t* /*unused*/, uint16_t devaddr, uint16_t /*unused*/)
{
    if (devaddr != device_address)
    {
        return;
    }
    usb_hstd_audio_stop();
}

} // namespace

extern "C" void usb_hstd_audio_register(usb_utr_t* ptr)
{
    usb_hstd_audio_stop();
    if (!deluge::usb::hostAudioEnabled())
    {
        return; // Leave composite devices available to the existing MIDI driver.
    }
    usb_hcdreg_t driver{};
    driver.ifclass    = USB_IFCLS_AUD;
    driver.ifsubclass = 0x01; // AudioControl; the parser then selects an AudioStreaming interface.
    driver.ifprotocol = USB_IFPROTO_ANY;
    driver.p_tpl      = target_peripheral_list.data();
    driver.p_pipetbl  = pipe_table.data();
    driver.classinit  = usb_hstd_dummy_function;
    driver.classcheck = classCheck;
    driver.devconfig  = configured;
    driver.devdetach  = detached;
    driver.devsuspend = usb_hstd_dummy_function;
    driver.devresume  = usb_hstd_dummy_function;
    usb_hstd_driver_registration(ptr, &driver);
}

extern "C" void usb_hstd_audio_stop(void)
{
    irq_streaming.store(false, std::memory_order_release);
    if (irq_transport_owned)
    {
        usb_hstd_audio_transport_stop(&audio_context);
        irq_transport_owned = false;
    }
    streaming        = false;
    audio_status     = USB_HOST_AUDIO_IDLE;
    received_packets = 0;
#if ENABLE_TEXT_OUTPUT
    max_irq_ticks.store(0, std::memory_order_relaxed);
#endif
    deluge::usb_audio::stopHostedInput();
    selected_stream.reset();
    device_address = USB_NODEVICE;
    device_speed   = USB_NOCONNECT;
}

extern "C" void usb_hstd_audio_detach_device(uint16_t address)
{
    if (address == device_address)
    {
        usb_hstd_audio_stop();
    }
}

extern "C" uint16_t usb_hstd_audio_handle_brdy(usb_utr_t* ptr, uint16_t bits)
{
    if (((bits & USB_BITSET(kAudioPipe)) == 0) || !irq_streaming.load(std::memory_order_acquire) || !selected_stream)
    {
        return 0;
    }
#if ENABLE_TEXT_OUTPUT
    uint16_t started = *TCNT[TIMER_SYSTEM_FAST];
#endif
    // Drain at most both FIFO planes per interrupt. No allocation, task messages, PIPESEL or CFIFO access here.
    for (unsigned packet = 0; packet < 2; ++packet)
    {
        int32_t received = usb_hstd_audio_transport_read(ptr, packet_buffer.data(), selected_stream->maxPacketBytes);
        if (received == -1)
        {
            break;
        }
        if (received < 0)
        {
            irq_streaming.store(false, std::memory_order_release);
            usb_hstd_audio_transport_stop(ptr);
            audio_status = USB_HOST_AUDIO_TRANSFER_ERROR;
            deluge::usb_audio::stopHostedInput();
            break;
        }
        publishPacket(static_cast<size_t>(received));
    }
#if ENABLE_TEXT_OUTPUT
    auto elapsed = static_cast<uint16_t>(*TCNT[TIMER_SYSTEM_FAST] - started);
    if (elapsed > max_irq_ticks.load(std::memory_order_relaxed))
    {
        max_irq_ticks.store(elapsed, std::memory_order_relaxed);
    }
#endif
    return USB_BITSET(kAudioPipe);
}

#if ENABLE_TEXT_OUTPUT
extern "C" uint32_t usb_hstd_audio_max_irq_ticks(void)
{
    return max_irq_ticks.load(std::memory_order_relaxed);
}
#endif

extern "C" usb_host_audio_status_t usb_hstd_audio_status(void)
{
    return audio_status;
}
extern "C" uint32_t usb_hstd_audio_packet_count(void)
{
    return received_packets;
}
