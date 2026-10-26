/*
 * Copyright © 2026 Synthstrom Audible Limited
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "RZA1/usb/r_usb_haudio/r_usb_haudio_if.h"
#include "drivers/usb/usb_mode.h"
#include "gui/menu_item/menu_item.h"
#include "gui/ui_timer_manager.h"
#include "hid/display/display.h"
#include "hid/display/oled.h"
#include <cstdio>

namespace deluge::gui::menu_item::usb {

class HostStatus final : public MenuItem {
public:
	using MenuItem::MenuItem;
	void beginSession(MenuItem*) override {
		firstRefresh = true;
		refresh();
	}
	ActionResult timerCallback() override {
		refresh();
		return ActionResult::DEALT_WITH;
	}
	void drawPixelsForOled() override {
		auto& canvas = hid::display::OLED::main;
		canvas.drawStringCentredShrinkIfNecessary(statusText(), 22, 18, 20);
		char count[32];
		snprintf(count, sizeof(count), "%s: %lu", l10n::get(l10n::String::STRING_FOR_USB_PACKETS),
		         static_cast<unsigned long>(usb_hstd_audio_packet_count()));
		canvas.drawStringCentredShrinkIfNecessary(count, 42, 12, 12);
	}

private:
	const char* statusText() {
		using enum l10n::String;
		if (!deluge::usb::hostMode()) {
			return l10n::get(STRING_FOR_USB_DEVICE_MODE);
		}
		auto text = STRING_FOR_USB_NO_AUDIO_DEVICE;
		switch (usb_hstd_audio_status()) {
		case USB_HOST_AUDIO_IDLE:
			break;
		case USB_HOST_AUDIO_UNSUPPORTED:
			text = STRING_FOR_USB_AUDIO_UNSUPPORTED;
			break;
		case USB_HOST_AUDIO_CONFIG_TOO_LARGE:
			text = STRING_FOR_USB_CONFIG_TOO_LARGE;
			break;
		case USB_HOST_AUDIO_PIPE_IN_USE:
			text = STRING_FOR_USB_AUDIO_PIPE_BUSY;
			break;
		case USB_HOST_AUDIO_CONFIGURING:
			text = STRING_FOR_USB_AUDIO_CONFIGURING;
			break;
		case USB_HOST_AUDIO_CLOCK_ERROR:
			text = STRING_FOR_USB_AUDIO_CLOCK_ERROR;
			break;
		case USB_HOST_AUDIO_INTERFACE_ERROR:
			text = STRING_FOR_USB_AUDIO_INTERFACE_ERROR;
			break;
		case USB_HOST_AUDIO_WAITING_PACKETS:
			text = STRING_FOR_USB_AUDIO_WAITING;
			break;
		case USB_HOST_AUDIO_RECEIVING:
			text = STRING_FOR_USB_AUDIO_RECEIVING;
			break;
		case USB_HOST_AUDIO_TRANSFER_ERROR:
			text = STRING_FOR_USB_AUDIO_TRANSFER_ERROR;
			break;
		}
		return l10n::get(text);
	}
	void refresh() {
		if (display->haveOLED()) {
			renderUIsForOled();
		}
		else {
			char text[100];
			snprintf(text, sizeof(text), "%s %lu", statusText(),
			         static_cast<unsigned long>(usb_hstd_audio_packet_count()));
			// Only restart scrolling when the stage changes; the live counter is on the OLED display.
			auto status = usb_hstd_audio_status();
			bool host = deluge::usb::hostMode();
			if (status != previousStatus || host != previousHost || firstRefresh) {
				display->setScrollingText(text);
			}
			previousStatus = status;
			previousHost = host;
		}
		firstRefresh = false;
		uiTimerManager.setTimer(TimerName::UI_SPECIFIC, 500);
	}
	usb_host_audio_status_t previousStatus = USB_HOST_AUDIO_IDLE;
	bool previousHost = false;
	bool firstRefresh = true;
};
} // namespace deluge::gui::menu_item::usb
