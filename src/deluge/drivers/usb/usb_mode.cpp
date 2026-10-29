/*
 * Copyright © 2026 Synthstrom Audible Limited
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "drivers/usb/usb_mode.h"

#include "RZA1/usb/r_usb_basic/r_usb_basic_if.h"
#include "RZA1/usb/r_usb_basic/src/hw/inc/r_usb_bitdefine.h"

extern "C" {
extern volatile uint32_t usbLock; // NOLINT(readability-identifier-naming): existing C interface.
extern uint16_t g_usb_usbmode;
extern uint8_t usbCurrentlyInitialized; // NOLINT(readability-identifier-naming): existing C interface.
uint16_t hw_usb_read_intsts();          // NOLINT(readability-identifier-naming): Renesas C interface.
void openUSBHost();
void closeUSBHost();
void openUSBPeripheral();
void closeUSBPeripheral();
}

namespace deluge::usb {
namespace {
int pending_mode = -1;
ModeChangeCallback pending_callback = nullptr;
bool audio_enabled = false;
bool pending_host_restart = false;
} // namespace

bool hostMode() {
	return (usbCurrentlyInitialized != 0u) && g_usb_usbmode == USB_HOST;
}

bool hostAudioEnabled() {
	return audio_enabled;
}

void setHostAudioEnabled(bool enabled) {
	if (audio_enabled != enabled) {
		audio_enabled = enabled;
		pending_host_restart = true;
	}
}

static ModeChangeResult changeMode(bool enabled, bool restartHost) {
	if ((usbCurrentlyInitialized != 0u) && enabled == hostMode() && !(enabled && restartHost)) {
		return ModeChangeResult::SUCCESS;
	}
	if (usbLock != 0u) {
		return ModeChangeResult::BUSY;
	}

	usbLock = 1;
	if (enabled && (usbCurrentlyInitialized != 0u) && !hostMode()) {
		// VBUS alone cannot identify another host: the Deluge's set/clear_vbout functions are no-ops.
		// A reset/address/configuration state is evidence that a host has actually enumerated this peripheral.
		// Mask out the suspend bit so suspended-but-only-powered does not look like enumeration.
		uint16_t status = hw_usb_read_intsts();
		if (((status & USB_VBSTS) != 0u) && ((status & USB_DVSQS) != 0u)) {
			usbLock = 0;
			return ModeChangeResult::HOST_CONNECTED;
		}
	}
	if (usbCurrentlyInitialized != 0u) {
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
	return (usbCurrentlyInitialized != 0u) && hostMode() == enabled ? ModeChangeResult::SUCCESS
	                                                                : ModeChangeResult::START_FAILED;
}

ModeChangeResult setHostMode(bool enabled) {
	return changeMode(enabled, false);
}

void requestHostMode(bool enabled, ModeChangeCallback callback) {
	pending_mode = static_cast<int>(enabled);
	pending_callback = callback;
}

bool requestedHostMode() {
	return pending_mode >= 0 ? pending_mode != 0 : hostMode();
}

void processPendingModeChange() {
	if ((pending_mode < 0 && !pending_host_restart) || (usbLock != 0u)) {
		return;
	}
	bool enabled = pending_mode >= 0 ? pending_mode != 0 : hostMode();
	bool restart_host = pending_host_restart;
	auto callback = pending_callback;
	pending_mode = -1;
	pending_callback = nullptr;
	pending_host_restart = false;
	auto result = changeMode(enabled, restart_host);
	// Opening USB can yield to the UI. Preserve a newer request made during that wait.
	if ((callback != nullptr) && pending_mode < 0) {
		callback(result);
	}
}

} // namespace deluge::usb
