/*
 * Copyright © 2026 Synthstrom Audible Limited
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "drivers/usb/usb_mode.h"

#include "RZA1/usb/r_usb_basic/r_usb_basic_if.h"
#include "RZA1/usb/r_usb_basic/src/hw/inc/r_usb_bitdefine.h"

extern "C" {
extern volatile uint32_t usbLock;
extern uint16_t g_usb_usbmode;
extern uint8_t usbCurrentlyInitialized;
uint16_t hw_usb_read_intsts();
void openUSBHost();
void closeUSBHost();
void openUSBPeripheral();
void closeUSBPeripheral();
}

namespace deluge::usb {
namespace {
int pendingMode = -1;
ModeChangeCallback pendingCallback = nullptr;
bool audioEnabled = false;
bool pendingHostRestart = false;
} // namespace

bool hostMode() {
	return usbCurrentlyInitialized && g_usb_usbmode == USB_HOST;
}

bool hostAudioEnabled() {
	return audioEnabled;
}

void setHostAudioEnabled(bool enabled) {
	if (audioEnabled != enabled) {
		audioEnabled = enabled;
		pendingHostRestart = true;
	}
}

static ModeChangeResult changeMode(bool enabled, bool restartHost) {
	if (usbCurrentlyInitialized && enabled == hostMode() && !(enabled && restartHost)) {
		return ModeChangeResult::Success;
	}
	if (usbLock) {
		return ModeChangeResult::Busy;
	}

	usbLock = 1;
	if (enabled && usbCurrentlyInitialized && !hostMode()) {
		// VBUS alone cannot identify another host: the Deluge's set/clear_vbout functions are no-ops.
		// A reset/address/configuration state is evidence that a host has actually enumerated this peripheral.
		// Mask out the suspend bit so suspended-but-only-powered does not look like enumeration.
		uint16_t status = hw_usb_read_intsts();
		if ((status & USB_VBSTS) && (status & USB_DVSQS)) {
			usbLock = 0;
			return ModeChangeResult::HostConnected;
		}
	}
	if (usbCurrentlyInitialized) {
		if (hostMode()) {
			closeUSBHost();
		}
		else {
			closeUSBPeripheral();
		}
	}
	if (enabled) {
		openUSBHost();
	}
	else {
		openUSBPeripheral();
	}
	usbLock = 0;
	return usbCurrentlyInitialized && hostMode() == enabled ? ModeChangeResult::Success : ModeChangeResult::StartFailed;
}

ModeChangeResult setHostMode(bool enabled) {
	return changeMode(enabled, false);
}

void requestHostMode(bool enabled, ModeChangeCallback callback) {
	pendingMode = enabled;
	pendingCallback = callback;
}

bool requestedHostMode() {
	return pendingMode >= 0 ? pendingMode != 0 : hostMode();
}

void processPendingModeChange() {
	if ((pendingMode < 0 && !pendingHostRestart) || usbLock) {
		return;
	}
	bool enabled = pendingMode >= 0 ? pendingMode != 0 : hostMode();
	bool restartHost = pendingHostRestart;
	auto callback = pendingCallback;
	pendingMode = -1;
	pendingCallback = nullptr;
	pendingHostRestart = false;
	auto result = changeMode(enabled, restartHost);
	// Opening USB can yield to the UI. Preserve a newer request made during that wait.
	if (callback && pendingMode < 0) {
		callback(result);
	}
}

} // namespace deluge::usb
