/* Copyright © 2026 Synthstrom Audible Limited. SPDX-License-Identifier: GPL-3.0-or-later */
#include "RZA1/usb/r_usb_haudio/r_usb_haudio_transport.h"
#include "RZA1/usb/r_usb_basic/src/driver/inc/r_usb_extern.h"
#include "RZA1/usb/r_usb_basic/src/hw/inc/r_usb_bitdefine.h"

uint16_t usb_hstd_audio_transport_start(usb_utr_t* ptr)
{
    // Setup runs in the USB task, before BRDY is enabled. PIPE2 was registered with DBLB and CNTMD=0,
    // so each FIFO plane contains a single packet and short packets do not terminate reception.
    ptr->ipp->PIPE2CTR &= (uint16_t)~USB_PID;
    usb_cstd_clr_transaction_counter(ptr, USB_PIPE2);
    ptr->ipp->D1FIFOSEL = USB_MBW_32 | USB_PIPE2;
    fifoSels[USB_D1USE] = USB_MBW_32 | USB_PIPE2;
    for (unsigned attempt = 0; attempt < 10000; ++attempt)
    {
        if ((ptr->ipp->D1FIFOSEL & (USB_CURPIPE | USB_MBW)) == (USB_MBW_32 | USB_PIPE2))
        {
            ptr->ipp->BRDYSTS = (uint16_t)~USB_BITSET(USB_PIPE2);
            ptr->ipp->BRDYENB |= USB_BITSET(USB_PIPE2);
            ptr->ipp->PIPE2CTR = (ptr->ipp->PIPE2CTR & (uint16_t)~USB_PID) | USB_PID_BUF;
            return USB_OK;
        }
    }
    usb_hstd_audio_transport_stop(ptr);
    return USB_ERROR;
}

void usb_hstd_audio_transport_stop(usb_utr_t* ptr)
{
    ptr->ipp->BRDYENB &= (uint16_t)~USB_BITSET(USB_PIPE2);
    ptr->ipp->PIPE2CTR &= (uint16_t)~USB_PID;
    // Bounded wait: no connected device may hold the firmware in this teardown path indefinitely.
    for (unsigned attempt = 0; attempt < 10000 && (ptr->ipp->PIPE2CTR & USB_PBUSY); ++attempt)
    {
    }
    ptr->ipp->D1FIFOSEL = USB_MBW_32;
    fifoSels[USB_D1USE] = USB_MBW_32;
    ptr->ipp->BRDYSTS   = (uint16_t)~USB_BITSET(USB_PIPE2);
}

int32_t usb_hstd_audio_transport_read(usb_utr_t* ptr, uint8_t* bytes, uint16_t capacity)
{
    // Acknowledge before releasing a FIFO plane, so a subsequent packet's BRDY is not erased.
    ptr->ipp->BRDYSTS = (uint16_t)~USB_BITSET(USB_PIPE2);
    uint16_t status   = ptr->ipp->D1FIFOCTR;
    if (!(status & USB_FRDY))
    {
        return -1;
    }
    uint16_t length = status & USB_DTLN;
    if (length > capacity)
    {
        ptr->ipp->D1FIFOCTR = USB_BCLR;
        return -2;
    }
    if (length == 0)
    {
        ptr->ipp->D1FIFOCTR = USB_BCLR;
    }
    else
    {
        // The caller supplies an aligned buffer. This helper handles a non-word-sized final fragment too.
        usb_hstd_read_fifo(ptr, length, USB_D1USE, bytes);
    }
    return length;
}
