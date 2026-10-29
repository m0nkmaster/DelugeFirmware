/*
 * Copyright © 2026 Synthstrom Audible Limited
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "drivers/usb/usb_mode.h"
#include "io/midi/midi_device_manager.h"
#include "io/midi/midi_engine.h"
#include <cstring>

extern "C" {
#include "RZA1/usb/r_usb_basic/r_usb_basic_if.h"
#include "RZA1/usb/r_usb_basic/src/driver/inc/r_usb_extern.h"
#include "RZA1/usb/r_usb_basic/src/hw/inc/r_usb_bitdefine.h"
#include "drivers/usb/userdef/r_usb_pmidi_config.h"
extern volatile uint32_t usbLock;
extern uint8_t usbCurrentlyInitialized;
extern uint16_t g_usb_usbmode;
extern uint16_t g_usb_peri_connected;
extern uint8_t stopSendingAfterDeviceNum[USB_NUM_USBIP];
extern uint8_t usbDeviceNumBeingSentToNow[USB_NUM_USBIP];
extern uint8_t anyUSBSendingStillHappening[USB_NUM_USBIP];
extern uint8_t currentDeviceNumWithSendPipe[USB_NUM_USBIP][2];
extern uint8_t usbHostOutPipeInitializedForHub[2];
extern usb_utr_t g_usb_midi_recv_utr[USB_NUM_USBIP][MAX_NUM_USB_MIDI_DEVICES];
extern uint16_t g_usb_hmidi_tmp_ep_tbl[USB_NUM_USBIP][MAX_NUM_USB_MIDI_DEVICES][USB_EPL * 2 + 1];
void usb_receive_start_rohan_midi(uint16_t pipe);
void usb_cstd_usb_task();
}
extern uint32_t timeLastBRDY[USB_NUM_USBIP];

// Called with usbLock held.
void MidiEngine::setupUSBHostReceiveTransfer(int32_t ip, int32_t midiDeviceNum) {
	connectedUSBMIDIDevices[ip][midiDeviceNum].currentlyWaitingToReceive = 1;

	int32_t pipeNumber = g_usb_hmidi_tmp_ep_tbl[USB_CFG_USE_USBIP][midiDeviceNum][USB_EPL];

	g_usb_midi_recv_utr[USB_CFG_USE_USBIP][midiDeviceNum].keyword = pipeNumber;
	// Arm one complete endpoint packet, including 512-byte high-speed bulk MIDI packets.
	g_usb_midi_recv_utr[USB_CFG_USE_USBIP][midiDeviceNum].tranlen =
	    g_usb_hmidi_tmp_ep_tbl[USB_CFG_USE_USBIP][midiDeviceNum][USB_EPL + 3] & USB_MXPS;

	g_p_usb_pipe[pipeNumber] = &g_usb_midi_recv_utr[USB_CFG_USE_USBIP][midiDeviceNum];
	usb_receive_start_rohan_midi(pipeNumber);
}

void MidiEngine::check_incoming_usb() {
	bool usbLockNow = usbLock;

	if (!usbLockNow) {
		// Have to call this regularly, to do "callbacks" that will grab out the received data
		usbLock = 1;
		usb_cstd_usb_task();
		usbLock = 0;
	}
}

void MidiEngine::checkIncomingUsbMidi() {

	deluge::usb::processPendingModeChange();
	if (!usbCurrentlyInitialized) {
		return;
	}
	bool usbLockNow = usbLock;
	check_incoming_usb();

	for (int32_t ip = 0; ip < USB_NUM_USBIP; ip++) {

		bool aPeripheral = (g_usb_usbmode != USB_HOST);
		if (aPeripheral && !g_usb_peri_connected) {
			continue;
		}
		// assumes only single device in peripheral mode
		int32_t numDevicesNow = aPeripheral ? 1 : MAX_NUM_USB_MIDI_DEVICES;

		for (int32_t d = 0; d < numDevicesNow; d++) {
			if (connectedUSBMIDIDevices[ip][d].cable[0] && !connectedUSBMIDIDevices[ip][d].currentlyWaitingToReceive) {

				int32_t bytesReceivedHere = connectedUSBMIDIDevices[ip][d].numBytesReceived;
				if (bytesReceivedHere) {
					connectedUSBMIDIDevices[ip][d].numBytesReceived = 0;

					uint8_t const* readPos = connectedUSBMIDIDevices[ip][d].receiveData;
					const uint8_t* const stopAt = readPos + bytesReceivedHere;

					// Receive all the stuff from this device
					for (; readPos < stopAt; readPos += 4) {

						uint8_t statusType = readPos[0] & 0x0F;
						uint8_t cable = (readPos[0] & 0xF0) >> 4;
						uint8_t channel = readPos[1] & 0x0F;
						uint8_t data1 = readPos[2];
						uint8_t data2 = readPos[3];
						if (statusType < 0x08) {
							if (statusType == 2 || statusType == 3) { // 2 or 3 byte system common messages
								statusType = 0x0F;
							}
							else { // Invalid, or sysex, or something
								checkIncomingUsbSysex(readPos, ip, d, cable);
								continue;
							}
						}
						if (data1 & 0x80 || data2 & 0x80) {
							// This shouldn't be possible for non-sysex messages, indicates an error in
							// transmission so just ignore the rest of the frame
							break;
						}
						// select appropriate device based on the cable number
						if (cable > connectedUSBMIDIDevices[ip][d].maxPortConnected) {
							// fallback to cable 0 since we don't support more than one port on hosted devices yet
							cable = 0;
						}
						midiMessageReceived(*connectedUSBMIDIDevices[ip][d].cable[cable], statusType, channel, data1,
						                    data2, &timeLastBRDY[ip]);
					}
				}

				if (usbLockNow) {
					continue;
				}

				// And maybe setup transfer to receive more data

				// As peripheral
				if (aPeripheral) {

					g_usb_midi_recv_utr[ip][0].keyword = USB_CFG_PMIDI_BULK_IN;
					g_usb_midi_recv_utr[ip][0].tranlen = 64;

					connectedUSBMIDIDevices[ip][0].currentlyWaitingToReceive = 1;

					usbLock = 1;
					g_p_usb_pipe[USB_CFG_PMIDI_BULK_IN] = &g_usb_midi_recv_utr[ip][0];
					usb_receive_start_rohan_midi(USB_CFG_PMIDI_BULK_IN);
					usbLock = 0;
				}

				// Or as host
				else if (connectedUSBMIDIDevices[ip][d].cable[0]) {

					// Only allowed to setup receive-transfer if not in the process of sending to various devices.
					// (Wait, still? Was this just because of that insane bug that's now fixed?)
					if (usbDeviceNumBeingSentToNow[ip] == stopSendingAfterDeviceNum[ip]) {
						usbLock = 1;
						setupUSBHostReceiveTransfer(ip, d);
						usbLock = 0;
					}
				}
			}
		}
	}
}

extern "C" void resetUsbMidiConnections() {
	for (int32_t ip = 0; ip < USB_NUM_USBIP; ++ip) {
		for (int32_t d = 0; d < MAX_NUM_USB_MIDI_DEVICES; ++d) {
			auto& connected = connectedUSBMIDIDevices[ip][d];
			for (auto*& cable : connected.cable) {
				if (cable) {
					cable->connectionFlags &= ~(1 << d);
					cable = nullptr;
				}
			}
			connected.setup();
			connected.ringBufReadIdx = connected.ringBufWriteIdx;
		}
		anyUSBSendingStillHappening[ip] = 0;
		usbDeviceNumBeingSentToNow[ip] = 0;
		stopSendingAfterDeviceNum[ip] = 0;
		currentDeviceNumWithSendPipe[ip][0] = MAX_NUM_USB_MIDI_DEVICES;
		currentDeviceNumWithSendPipe[ip][1] = MAX_NUM_USB_MIDI_DEVICES;
	}
	usbHostOutPipeInitializedForHub[0] = 0;
	usbHostOutPipeInitializedForHub[1] = 0;
	anythingInUSBOutputBuffer = false;
	MIDIDeviceManager::recountSmallestMPEZones();
}

void ConnectedUSBMIDIDevice::setup() {
	numBytesSendingNow = 0;
	currentlyWaitingToReceive = false;
	numBytesReceived = 0;

	// default to only a single port
	maxPortConnected = 0;
}

ConnectedUSBMIDIDevice::ConnectedUSBMIDIDevice() {
	currentlyWaitingToReceive = 0;
	sq = 0;
	canHaveMIDISent = 0;
	numBytesReceived = 0;
	memset(receiveData, 0, sizeof(receiveData));
	memset(dataSendingNow, 0, MIDI_SEND_BUFFER_LEN_INNER * 4);
	numBytesSendingNow = 0;
	memset(sendDataRingBuf, 0, MIDI_SEND_BUFFER_LEN_RING);
	ringBufWriteIdx = 0;
	ringBufReadIdx = 0;

	maxPortConnected = 0;
}
