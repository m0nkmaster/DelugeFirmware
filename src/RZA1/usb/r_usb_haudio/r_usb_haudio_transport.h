/* Copyright © 2026 Synthstrom Audible Limited. SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "RZA1/usb/r_usb_basic/r_usb_basic_if.h"

// Public names follow the Renesas C interface.
// NOLINTBEGIN(readability-identifier-naming)
#ifdef __cplusplus
extern "C" {
#endif

// Owns PIPE2 and D1FIFO until stopped. No control/MIDI CFIFO access from the audio interrupt.
uint16_t usb_hstd_audio_transport_start(usb_utr_t* ptr);
void usb_hstd_audio_transport_stop(usb_utr_t* ptr);
// One packet, or -1 when no buffer is ready, -2 for oversized data. Never waits for hardware in the ISR.
int32_t usb_hstd_audio_transport_read(usb_utr_t* ptr, uint8_t* bytes, uint16_t capacity);

#ifdef __cplusplus
}
#endif

// NOLINTEND(readability-identifier-naming)
