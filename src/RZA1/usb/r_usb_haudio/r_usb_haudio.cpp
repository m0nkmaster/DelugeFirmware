/*
 * Copyright © 2026 Synthstrom Audible Limited
 *
 * Host-side USB Audio capture for the Deluge. The Renesas stack calls the class callbacks from its cooperative USB
 * task. Audio packets are converted there and handed to the lock-free input ring consumed by the audio engine.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "RZA1/usb/r_usb_haudio/r_usb_haudio_if.h"

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
#include "deluge/io/debug/log.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace {

constexpr uint16_t kAudioPipe         = USB_PIPE2;
constexpr uint16_t kAudioBufferNumber = 16; // Blocks 16..31, beyond the MIDI/hub buffers at 0..12.
constexpr size_t kMaxPacketBytes      = 1023;
constexpr size_t kMaxDecodedSamples   = kMaxPacketBytes * 2;

const uint16_t targetPeripheralList[] = {
    USB_CFG_TPLCNT,
    0,
    USB_CFG_TPL,
};

uint16_t pipeTable[USB_EPL + 1] = {
    kAudioPipe,
    USB_NULL,
    static_cast<uint16_t>(USB_BUF_SIZE(1024u) | USB_BUF_NUMB(kAudioBufferNumber)),
    USB_NULL,
    USB_NULL,
    USB_NULL,
    USB_PDTBLEND,
};

std::optional<deluge::hosted_audio::InputStream> selectedStream;
uint16_t deviceAddress = USB_NODEVICE;
uint16_t deviceSpeed   = USB_NOCONNECT;
bool streaming         = false;

alignas(4) std::array<uint8_t, kMaxPacketBytes + 1> packetBuffer;
PLACE_SDRAM_BSS std::array<int32_t, kMaxDecodedSamples> decodedSamples;
usb_utr_t streamTransfer{};
usb_utr_t controlTransfer{};
uint16_t controlSetup[5]{};
std::array<uint8_t, 4> sampleRateData{};
uint32_t requestedBytes = 0;

uint16_t configurationLength(const uint8_t* configuration)
{
    return static_cast<uint16_t>(configuration[2]) | static_cast<uint16_t>(configuration[3]) << 8;
}

const uint8_t* findEndpoint(
    const uint8_t* configuration, uint16_t length, const deluge::hosted_audio::InputStream& stream)
{
    bool inSelectedAlternate = false;
    for (uint16_t offset = 0; offset + 2 <= length;)
    {
        uint8_t descriptorLength = configuration[offset];
        if (descriptorLength < 2 || offset + descriptorLength > length)
        {
            return nullptr;
        }
        const uint8_t* descriptor = configuration + offset;
        if (descriptor[1] == USB_DT_INTERFACE && descriptorLength >= 9)
        {
            inSelectedAlternate = descriptor[2] == stream.interfaceNumber && descriptor[3] == stream.alternateSetting;
        }
        else if (inSelectedAlternate && descriptor[1] == USB_DT_ENDPOINT && descriptorLength >= 7
                 && descriptor[2] == stream.endpointAddress)
        {
            return descriptor;
        }
        offset += descriptorLength;
    }
    return nullptr;
}

void queuePacket();
void selectStreamingInterface(usb_utr_t* ptr);

void packetComplete(usb_utr_t* transfer, uint16_t, uint16_t)
{
    if (!streaming || !selectedStream)
    {
        return;
    }

    if (transfer->status != USB_DATA_READING)
    {
        // In particular, detach/forced termination must not submit another transfer to a disappearing device.
        streaming = false;
        deluge::usb_audio::stopHostedInput();
        return;
    }

    uint32_t remaining = std::min<uint32_t>(transfer->tranlen, requestedBytes);
    uint32_t received  = requestedBytes - remaining;
    size_t frameBytes  = selectedStream->channels * selectedStream->subslotBytes;
    if (received != 0 && received % frameBytes == 0)
    {
        size_t frames = deluge::hosted_audio::decodePcm(
            std::span<const uint8_t>{packetBuffer.data(), received}, *selectedStream, decodedSamples);
        deluge::usb_audio::receiveHostedInput(std::span<const int32_t>{decodedSamples.data(), frames * 2});
    }

    queuePacket();
}

void queuePacket()
{
    if (!streaming || !selectedStream)
    {
        return;
    }

    requestedBytes           = std::min<uint32_t>(selectedStream->maxPacketBytes, packetBuffer.size());
    streamTransfer.keyword   = kAudioPipe;
    streamTransfer.p_tranadr = packetBuffer.data();
    streamTransfer.tranlen   = requestedBytes;
    streamTransfer.complete  = packetComplete;
    streamTransfer.segment   = USB_TRAN_END;
    streamTransfer.ip        = USB_CFG_USE_USBIP;
    streamTransfer.ipp       = usb_hstd_get_usb_ip_adr(USB_CFG_USE_USBIP);

    if (usb_hstd_transfer_start(&streamTransfer) != USB_OK)
    {
        streaming = false;
        deluge::usb_audio::stopHostedInput();
        D_PRINTLN("USB host audio: failed to queue capture packet");
    }
}

void startStreaming()
{
    if (!selectedStream || deviceAddress == USB_NODEVICE)
    {
        return;
    }
    streaming = true;
    deluge::usb_audio::startHostedInput();
    queuePacket();
}

void readSampleRate(usb_utr_t* ptr);

void sampleRateReadComplete(usb_utr_t* transfer, uint16_t, uint16_t)
{
    if (!selectedStream || deviceAddress == USB_NODEVICE)
    {
        return;
    }
    uint32_t actualRate = static_cast<uint32_t>(sampleRateData[0]) | static_cast<uint32_t>(sampleRateData[1]) << 8
                          | static_cast<uint32_t>(sampleRateData[2]) << 16
                          | static_cast<uint32_t>(sampleRateData[3]) << 24;
    if (transfer->status != USB_CTRL_END || transfer->tranlen != 0 || actualRate != selectedStream->sampleRate)
    {
        D_PRINTLN("USB host audio: device rejected 44.1kHz clock");
        return;
    }
    if (selectedStream->version == deluge::hosted_audio::AudioClassVersion::UAC2)
    {
        selectStreamingInterface(transfer);
    }
    else
    {
        startStreaming();
    }
}

void sampleRateComplete(usb_utr_t* transfer, uint16_t, uint16_t)
{
    if (!selectedStream || deviceAddress == USB_NODEVICE)
    {
        return;
    }
    if (transfer->status != USB_CTRL_END)
    {
        D_PRINTLN("USB host audio: sample-rate request failed");
        return;
    }
    readSampleRate(transfer);
}

void readSampleRate(usb_utr_t* ptr)
{
    bool uac2 = selectedStream->version == deluge::hosted_audio::AudioClassVersion::UAC2;
    sampleRateData.fill(0);
    controlSetup[0] = (uac2 ? 0x0100u : 0x8100u) | USB_DEV_TO_HOST | USB_CLASS | (uac2 ? USB_INTERFACE : USB_ENDPOINT);
    controlSetup[1] = 0x0100u;
    controlSetup[2] =
        uac2 ? static_cast<uint16_t>(selectedStream->clockSourceId << 8 | selectedStream->controlInterfaceNumber)
             : selectedStream->endpointAddress;
    controlSetup[3]           = uac2 ? 4 : 3;
    controlSetup[4]           = deviceAddress;
    controlTransfer.p_tranadr = sampleRateData.data();
    controlTransfer.complete  = sampleRateReadComplete;
    controlTransfer.tranlen   = controlSetup[3];
    controlTransfer.keyword   = USB_PIPE0;
    controlTransfer.p_setup   = controlSetup;
    controlTransfer.segment   = USB_TRAN_END;
    controlTransfer.ip        = ptr->ip;
    controlTransfer.ipp       = ptr->ipp;
    if (usb_hstd_transfer_start(&controlTransfer) != USB_OK)
    {
        D_PRINTLN("USB host audio: couldn't read sample rate");
    }
}

void setSampleRate(usb_utr_t* ptr)
{
    uint32_t rate     = selectedStream->sampleRate;
    sampleRateData[0] = rate;
    sampleRateData[1] = rate >> 8;
    sampleRateData[2] = rate >> 16;
    sampleRateData[3] = rate >> 24;

    controlSetup[0] =
        0x0100u | USB_HOST_TO_DEV | USB_CLASS
        | (selectedStream->version == deluge::hosted_audio::AudioClassVersion::UAC2 ? USB_INTERFACE : USB_ENDPOINT);
    controlSetup[1] = 0x0100u; // SAM_FREQ_CONTROL
    controlSetup[2] =
        selectedStream->version == deluge::hosted_audio::AudioClassVersion::UAC2
            ? static_cast<uint16_t>(selectedStream->clockSourceId << 8 | selectedStream->controlInterfaceNumber)
            : selectedStream->endpointAddress;
    controlSetup[3] = selectedStream->version == deluge::hosted_audio::AudioClassVersion::UAC2 ? 4 : 3;
    controlSetup[4] = deviceAddress;

    controlTransfer.p_tranadr = sampleRateData.data();
    controlTransfer.complete  = sampleRateComplete;
    controlTransfer.tranlen   = controlSetup[3];
    controlTransfer.keyword   = USB_PIPE0;
    controlTransfer.p_setup   = controlSetup;
    controlTransfer.segment   = USB_TRAN_END;
    controlTransfer.ip        = ptr->ip;
    controlTransfer.ipp       = ptr->ipp;
    if (usb_hstd_transfer_start(&controlTransfer) != USB_OK)
    {
        D_PRINTLN("USB host audio: couldn't set sample rate");
    }
}

void setInterfaceComplete(usb_utr_t* transfer, uint16_t, uint16_t)
{
    if (!selectedStream || deviceAddress == USB_NODEVICE || transfer->status != USB_CTRL_END)
    {
        return;
    }
    if (selectedStream->version == deluge::hosted_audio::AudioClassVersion::UAC1
        && selectedStream->sampleRateNeedsControlRequest)
    {
        setSampleRate(transfer);
        return;
    }
    startStreaming();
}

void selectStreamingInterface(usb_utr_t* ptr)
{
    controlSetup[0] = USB_SET_INTERFACE | USB_HOST_TO_DEV | USB_STANDARD | USB_INTERFACE;
    controlSetup[1] = selectedStream->alternateSetting;
    controlSetup[2] = selectedStream->interfaceNumber;
    controlSetup[3] = 0;
    controlSetup[4] = deviceAddress;

    controlTransfer.p_tranadr = nullptr;
    controlTransfer.complete  = setInterfaceComplete;
    controlTransfer.tranlen   = 0;
    controlTransfer.keyword   = USB_PIPE0;
    controlTransfer.p_setup   = controlSetup;
    controlTransfer.segment   = USB_TRAN_END;
    controlTransfer.ip        = ptr->ip;
    controlTransfer.ipp       = ptr->ipp;
    if (usb_hstd_transfer_start(&controlTransfer) != USB_OK)
    {
        D_PRINTLN("USB host audio: couldn't select streaming interface");
    }
}

void classCheck(usb_utr_t* ptr, uint16_t** table)
{
    *table[3] = USB_ERROR;
    if (deviceAddress != USB_NODEVICE || *table[6] != USB_FSCONNECT
        || (g_usb_hstd_use_pipe[ptr->ip] & USB_BITSET(kAudioPipe)))
    {
        return; // One capture stream; never overwrite a pipe already allocated to a MIDI device.
    }
    auto* configuration    = reinterpret_cast<uint8_t*>(table[1]);
    auto* controlInterface = reinterpret_cast<uint8_t*>(table[2]);
    uint16_t length        = std::min<uint16_t>(configurationLength(configuration), USB_CONFIGSIZE);
    selectedStream         = deluge::hosted_audio::findInputStream(
        {configuration, length}, deluge::hosted_audio::kDelugeSampleRate, controlInterface[2]);
    if (!selectedStream || selectedStream->maxPacketBytes > kMaxPacketBytes)
    {
        *table[3] = USB_ERROR;
        return;
    }

    const uint8_t* endpoint = findEndpoint(configuration, length, *selectedStream);
    if (!endpoint)
    {
        *table[3] = USB_ERROR;
        return;
    }

    deviceSpeed  = *table[6];
    pipeTable[1] = USB_NULL;
    pipeTable[2] = static_cast<uint16_t>(USB_BUF_SIZE(1024u) | USB_BUF_NUMB(kAudioBufferNumber));
    pipeTable[3] = USB_NULL;
    pipeTable[4] = USB_NULL;
    if (usb_hstd_chk_pipe_info(deviceSpeed, pipeTable, const_cast<uint8_t*>(endpoint)) == USB_ERROR)
    {
        selectedStream.reset();
        *table[3] = USB_ERROR;
        return;
    }

    deviceAddress = *table[7];
    *table[3]     = USB_OK;
    usb_hstd_return_enu_mgr(ptr, USB_OK);
}

void configured(usb_utr_t* ptr, uint16_t devaddr, uint16_t)
{
    if (!selectedStream || devaddr != deviceAddress)
    {
        return;
    }
    pipeTable[3] |= static_cast<uint16_t>(devaddr << USB_DEVADDRBIT);
    usb_hstd_set_pipe_registration(ptr, pipeTable, USB_USEPIPE);
    if (selectedStream->version == deluge::hosted_audio::AudioClassVersion::UAC2)
    {
        if (selectedStream->clockFrequencyControls == 3)
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

void detached(usb_utr_t*, uint16_t devaddr, uint16_t)
{
    if (devaddr != deviceAddress)
    {
        return;
    }
    usb_hstd_audio_stop();
}

} // namespace

extern "C" void usb_hstd_audio_register(usb_utr_t* ptr)
{
    usb_hstd_audio_stop();
    usb_hcdreg_t driver{};
    driver.ifclass    = USB_IFCLS_AUD;
    driver.ifsubclass = 0x01; // AudioControl; the parser then selects an AudioStreaming interface.
    driver.ifprotocol = USB_IFPROTO_ANY;
    driver.p_tpl      = const_cast<uint16_t*>(targetPeripheralList);
    driver.p_pipetbl  = pipeTable;
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
    streaming = false;
    deluge::usb_audio::stopHostedInput();
    selectedStream.reset();
    deviceAddress = USB_NODEVICE;
    deviceSpeed   = USB_NOCONNECT;
}
