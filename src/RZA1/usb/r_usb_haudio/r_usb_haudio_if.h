/*
 * Copyright © 2026 Synthstrom Audible Limited
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "RZA1/usb/r_usb_basic/r_usb_basic_if.h"

#ifdef __cplusplus
extern "C" {
#endif

void usb_hstd_audio_register(usb_utr_t* ptr);
// Called before closing host mode; cancels the class state before any outstanding transfers are torn down.
void usb_hstd_audio_stop(void);
// Release the selected audio device before the HCD clears its pipe configuration.
void usb_hstd_audio_detach_device(uint16_t address);
// Called directly from the USB ISR. Returns the BRDY bits consumed without posting a cooperative task message.
uint16_t usb_hstd_audio_handle_brdy(usb_utr_t* ptr, uint16_t bits);

typedef enum {
    USB_HOST_AUDIO_IDLE,
    USB_HOST_AUDIO_UNSUPPORTED,
    USB_HOST_AUDIO_CONFIG_TOO_LARGE,
    USB_HOST_AUDIO_PIPE_IN_USE,
    USB_HOST_AUDIO_CONFIGURING,
    USB_HOST_AUDIO_CLOCK_ERROR,
    USB_HOST_AUDIO_INTERFACE_ERROR,
    USB_HOST_AUDIO_WAITING_PACKETS,
    USB_HOST_AUDIO_RECEIVING,
    USB_HOST_AUDIO_TRANSFER_ERROR,
} usb_host_audio_status_t;

usb_host_audio_status_t usb_hstd_audio_status(void);
uint32_t usb_hstd_audio_packet_count(void);
#if ENABLE_TEXT_OUTPUT
// Maximum audio handler time in TIMER_SYSTEM_FAST ticks since stream teardown. No logging in the ISR.
uint32_t usb_hstd_audio_max_irq_ticks(void);
#endif

#ifdef __cplusplus
}
#endif
