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

#ifdef __cplusplus
}
#endif
