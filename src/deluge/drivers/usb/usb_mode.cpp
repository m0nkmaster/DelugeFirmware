/*
 * Copyright © 2026 Synthstrom Audible Limited
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "drivers/usb/usb_mode.h"

#include "RZA1/usb/r_usb_basic/r_usb_basic_if.h"

extern "C" {
extern volatile uint32_t usbLock;
extern uint16_t g_usb_usbmode;
extern uint8_t usbCurrentlyInitialized;
uint16_t usb_pstd_chk_vbsts();
void openUSBHost();
void closeUSBHost();
void openUSBPeripheral();
void closeUSBPeripheral();
}

namespace deluge::usb {

bool hostMode() {
	return usbCurrentlyInitialized && g_usb_usbmode == USB_HOST;
}

bool setHostMode(bool enabled) {
	if (usbCurrentlyInitialized && enabled == hostMode()) {
		return true;
	}
	if (usbLock) {
		return false;
	}

	usbLock = 1;
	// Do not drive VBUS against a computer or another externally powered USB host.
	if (enabled && usbCurrentlyInitialized && !hostMode() && usb_pstd_chk_vbsts() == USB_ATTACH) {
		usbLock = 0;
		return false;
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
	return usbCurrentlyInitialized && hostMode() == enabled;
}

} // namespace deluge::usb
