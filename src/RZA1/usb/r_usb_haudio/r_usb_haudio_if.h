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

#ifdef __cplusplus
}
#endif
