/***********************************************************************************************************************
 * DISCLAIMER
 * This software is supplied by Renesas Electronics Corporation and is only intended for use with Renesas products. No
 * other uses are authorized. This software is owned by Renesas Electronics Corporation and is protected under all
 * applicable laws, including copyright laws.
 * THIS SOFTWARE IS PROVIDED "AS IS" AND RENESAS MAKES NO WARRANTIES REGARDING
 * THIS SOFTWARE, WHETHER EXPRESS, IMPLIED OR STATUTORY, INCLUDING BUT NOT LIMITED TO WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. ALL SUCH WARRANTIES ARE EXPRESSLY DISCLAIMED. TO THE MAXIMUM
 * EXTENT PERMITTED NOT PROHIBITED BY LAW, NEITHER RENESAS ELECTRONICS CORPORATION NOR ANY OF ITS AFFILIATED COMPANIES
 * SHALL BE LIABLE FOR ANY DIRECT, INDIRECT, SPECIAL, INCIDENTAL OR CONSEQUENTIAL DAMAGES FOR ANY REASON RELATED TO THIS
 * SOFTWARE, EVEN IF RENESAS OR ITS AFFILIATES HAVE BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGES.
 * Renesas reserves the right, without notice, to make changes to this software and to discontinue the availability of
 * this software. By using this software, you agree to the additional terms and conditions found by accessing the
 * following link:
 * http://www.renesas.com/disclaimer
 *
 * Copyright (C) 2016 Renesas Electronics Corporation. All rights reserved.
 ***********************************************************************************************************************/
#include "RZA1/usb/r_usb_basic/r_usb_basic_if.h"
#include "RZA1/usb/r_usb_basic/src/driver/inc/r_usb_extern.h"
#include "RZA1/usb/r_usb_basic/src/hw/inc/r_usb_reg_access.h"
#include "RZA1/usb/userdef/r_usb_hmidi_config.h"
#include "deluge/io/midi/midi_device_manager.h"
extern usb_utr_t g_usb_midi_recv_utr[USB_NUM_USBIP][MAX_NUM_USB_MIDI_DEVICES];

void usb_hstd_brdy_pipe_process_rohan_midi_and_hub(usb_utr_t* ptr, uint16_t bitsts)
{
    for (uint16_t pipe = USB_CFG_HMIDI_BULK_RECV_MIN; pipe <= USB_PIPE9; pipe++)
    {
        if (pipe == USB_CFG_HMIDI_BULK_RECV_MAX + 1)
        {
            pipe = USB_CFG_HMIDI_INT_RECV_MIN;
        }
        if (!(bitsts & USB_BITSET(pipe)))
        {
            continue;
        }
        // Audio needs its class callback and actual packet length; MIDI bypasses class callbacks below.
        if (usb_cstd_get_pipe_type(ptr, pipe) == USB_TYPFIELD_ISO)
        {
            usb_hstd_brdy_pipe_process(ptr, USB_BITSET(pipe));
            continue;
        }
        g_usb_hstd_ignore_cnt[USB_CFG_USE_USBIP][pipe] = 0;
        uint16_t end_flag                              = usb_read_data_fast_rohan(pipe);
        if (end_flag != USB_READEND)
        {
            usb_hstd_forced_termination(ptr, pipe, USB_DATA_ERR);
            continue;
        }
        if (pipe == USB_PIPE9)
        {
            // Hubs need the normal callback to detect devices connected after startup.
            usb_hstd_data_end(ptr, pipe, USB_DATA_OK);
            continue;
        }
        usb_utr_t* temp         = g_p_usb_pipe[pipe];
        unsigned int device_num = temp - &g_usb_midi_recv_utr[0][0];
        if (device_num < MAX_NUM_USB_MIDI_DEVICES)
        {
            connectedUSBMIDIDevices[0][device_num].numBytesReceived = temp->tranlen - g_usb_data_cnt[pipe];
            // Zero-length packets occur with some devices; still allow the next receive to be armed.
            connectedUSBMIDIDevices[0][device_num].currentlyWaitingToReceive = 0;
        }
    }
}
