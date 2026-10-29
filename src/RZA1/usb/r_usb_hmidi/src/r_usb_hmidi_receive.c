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
    // uint16_t    useport;
    uint16_t pipe;
    // uint16_t    ip;
#if ((USB_CFG_DTC == USB_CFG_ENABLE) || (USB_CFG_DMA == USB_CFG_ENABLE))
    uint16_t buffer;
    uint16_t maxps;
    uint16_t set_dtc_brock_cnt;
    uint16_t trans_dtc_block_cnt;
    uint16_t dma_ch;
    uint16_t status;
#endif /* ((USB_CFG_DTC == USB_CFG_ENABLE) || (USB_CFG_DMA == USB_CFG_ENABLE)) */

    // ip = ptr->ip;
    for (pipe = USB_CFG_HMIDI_BULK_RECV_MIN; pipe <= USB_PIPE9; pipe++)
    { // We'll just do the receive pipes we actually sometimes use for MIDI, plus PIPE9 for hubs. We could also do it by
      // USB device that we know is connected...
        if (pipe == USB_CFG_HMIDI_BULK_RECV_MAX + 1)
            pipe = USB_CFG_HMIDI_INT_RECV_MIN; // Skip that one pipe

        if (bitsts & USB_BITSET(pipe))
        {
            // Audio pipes need the generic completion callback and their actual packet length. The MIDI fast path
            // below intentionally bypasses class callbacks.
            if (usb_cstd_get_pipe_type(ptr, pipe) == USB_TYPFIELD_ISO)
            {
                usb_hstd_brdy_pipe_process(ptr, USB_BITSET(pipe));
                continue;
            }
            /* Interrupt check */
            // hw_usb_clear_status_bemp(NULL, pipe);

            if (true || USB_NULL != g_p_usb_pipe[pipe])
            { // I think this'll be fine now
                /* Pipe number to FIFO port select */
                // useport = USB_CUSE;//usb_hstd_pipe2fport(ptr, i);

                /* FIFO to Buffer data read */
                // usb_hstd_fifo_to_buf(ptr, pipe, useport);
                {
                    uint16_t end_flag;

                    /* Ignore count clear */
                    g_usb_hstd_ignore_cnt[USB_CFG_USE_USBIP][pipe] = (uint16_t)0;

                    end_flag = usb_read_data_fast_rohan(
                        pipe); // Modified by Rohan to call the PSTD one, which does the exact same now

                    if (end_flag == USB_READEND)
                    { // I condensed USB_READSHRT into this too
#if 1
                        if (pipe == USB_PIPE9)
                        { // For hub - use original function. Without this, reconnecting devices to hub doesn't work,
                          // even though they do if initially connected.
                            usb_hstd_data_end(ptr, pipe, USB_DATA_OK);
                        }
                        else
#endif
                        { // For MIDI - use this optimized code

#if 0
							    /* PID = NAK */
							    /* Set NAK */
							    //usb_cstd_select_nak(ptr, pipe);
								usb_cstd_set_nak_fast_rohan(pipe);

							    /* Disable Interrupt */
							    /* Disable Ready Interrupt */
							    hw_usb_clear_brdyenb(NULL, pipe);

							    /* Disable Not Ready Interrupt */
							    hw_usb_clear_nrdyenb(NULL, pipe);

							    /* Disable Empty Interrupt */
							    hw_usb_clear_bempenb(NULL, pipe);

							    /* Disable Transaction count */
							    //usb_cstd_clr_transaction_counter(ptr, pipe);
							    hw_usb_clear_trenb(NULL, pipe);
							    hw_usb_set_trclr(NULL, pipe);
#endif

                            /* Call Back */
                            if (true || USB_NULL != g_p_usb_pipe[pipe]) // Checked above already
                            {
                                /* Check PIPE TYPE */
                                if (true || USB_TYPFIELD_ISO != usb_cstd_get_pipe_type(NULL, pipe))
                                {
                                    /* Transfer information set */
                                    /* // Doesn't get read, anyway
                                        g_p_usb_pipe[pipe]->tranlen    = g_usb_data_cnt[pipe];
                                        g_p_usb_pipe[pipe]->status     = newStatus;
                                        g_p_usb_pipe[pipe]->pipectr    = hw_usb_read_pipectr(NULL, pipe);
                                        g_p_usb_pipe[pipe]->errcnt     = (uint8_t)
                                       g_usb_hstd_ignore_cnt[USB_CFG_USE_USBIP][pipe]; g_p_usb_pipe[pipe]->ipp        =
                                       usb_hstd_get_usb_ip_adr(USB_CFG_USE_USBIP); g_p_usb_pipe[pipe]->ip         =
                                       USB_CFG_USE_USBIP;
                                        */
                                    usb_utr_t* temp = g_p_usb_pipe[pipe];
                                    // g_p_usb_pipe[pipe] = (usb_utr_t*) USB_NULL; // Moved by Rohan - set this to NULL
                                    // before the callback so the callback can do another transfer, which sets this to
                                    // something else (temp->complete)(temp, 0, 0); // This does our callback on our
                                    // outgoing transfers. Rohan

                                    unsigned int deviceNum = temp - &g_usb_midi_recv_utr[0][0];

                                    if (deviceNum < MAX_NUM_USB_MIDI_DEVICES)
                                    { // Gotta check this - I can totally see something going wrong, with all the other
                                      // checks I'm now skipping!
                                        connectedUSBMIDIDevices[0][deviceNum].numBytesReceived =
                                            temp->tranlen - g_usb_data_cnt[pipe];
                                        // Warning - sometimes (with a Teensy, e.g. my knob box), length will be 0. Not
                                        // sure why - but we need to cope with that case.

                                        connectedUSBMIDIDevices[0][deviceNum].currentlyWaitingToReceive =
                                            0; // Take note that we need to set up another receive
                                    }
                                }
                            }
                        }
                    }
                    else
                    { // USB_FIFOERROR and formerly USB_READOVER
                        usb_hstd_forced_termination(ptr, pipe, USB_DATA_ERR);
                    }
                }
            }
        }
    }
}
