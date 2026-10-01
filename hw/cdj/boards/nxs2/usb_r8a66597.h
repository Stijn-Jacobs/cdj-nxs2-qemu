/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Register map of the SH7724 USB 2.0 host/function module (manual section
 * 31.4), a Renesas R8A66597-family controller. USB0 and USB1 are two
 * instances of it; only the registers the firmware touches are named.
 */
#ifndef CDJ_USB_R8A66597_H
#define CDJ_USB_R8A66597_H

#define CDJ_USB_SYSCFG   0x00
#define CDJ_USB_SYSSTS0  0x04
#define CDJ_USB_DVSTCTR0 0x08
/* Three windows onto the pipe buffers, each with its own CURPIPE: CFIFO is the
 * CPU's, D0FIFO and D1FIFO are wired to the DMAC. */
#define CDJ_USB_CFIFO    0x14
#define CDJ_USB_CFIFOSEL 0x20
#define CDJ_USB_CFIFOCTR 0x22
#define CDJ_USB_D0FIFOSEL 0x28
#define CDJ_USB_D0FIFOCTR 0x2A
#define CDJ_USB_D1FIFOSEL 0x2C
#define CDJ_USB_D1FIFOCTR 0x2E
#define CDJ_USB_D0FIFO   0x100
#define CDJ_USB_D1FIFO   0x120
#define CDJ_USB_D0FIFO_ALT 0x18
#define CDJ_USB_D1FIFO_ALT 0x1C
#define CDJ_USB_NR_FIFO_PORTS 3
#define CDJ_USB_INTENB0  0x30
#define CDJ_USB_INTENB1  0x32
#define CDJ_USB_INTSTS0  0x40
#define CDJ_USB_INTSTS1  0x42
#define CDJ_USB_BRDYENB  0x36
#define CDJ_USB_NRDYENB  0x38
#define CDJ_USB_BEMPENB  0x3A
#define CDJ_USB_BRDYSTS  0x46
#define CDJ_USB_NRDYSTS  0x48
#define CDJ_USB_BEMPSTS  0x4A
#define CDJ_USB_FRMNUM   0x4C
#define CDJ_USB_USBADDR  0x50
#define CDJ_USB_USBREQ   0x54
#define CDJ_USB_USBVAL   0x56
#define CDJ_USB_USBINDX  0x58
#define CDJ_USB_USBLENG  0x5A
#define CDJ_USB_DCPCFG   0x5C
#define CDJ_USB_DCPMAXP  0x5E
#define CDJ_USB_DCPCTR   0x60
#define CDJ_USB_PIPESEL  0x64
#define CDJ_USB_PIPECFG  0x68
#define CDJ_USB_PIPEBUF  0x6A
#define CDJ_USB_PIPEMAXP 0x6C
#define CDJ_USB_PIPEPERI 0x6E
/* PIPE1CTR..PIPE9CTR are consecutive from +0x70. */
#define CDJ_USB_PIPECTR(n) (0x70 + 2 * ((n) - 1))
/* Transaction counters, pipes 1..5 only: TRE bit 9 TRENB, bit 8 TRCLR. */
#define CDJ_USB_PIPETRE(n) (0x90 + 4 * ((n) - 1))
#define CDJ_USB_PIPETRN(n) (0x92 + 4 * ((n) - 1))
#define CDJ_USB_NR_PIPES 10          /* index 0 is the DCP */

/* INTSTS0 bit 8 BRDY / bit 9 NRDY / bit 10 BEMP: the per-pipe FIFO events.
 * BRDYSTS/NRDYSTS/BEMPSTS carry which pipe, one bit each, pipe 0 = the DCP. */
#define CDJ_USB_BRDY     0x0100
#define CDJ_USB_NRDY     0x0200
#define CDJ_USB_BEMP     0x0400

/* DCPCTR and PIPEnCTR: bit 15 BSTS, 1:0 PID; DCPCTR bit 2 CCPL. */
#define CDJ_USB_BSTS     0x8000
#define CDJ_USB_CCPL     0x0004
#define CDJ_USB_PID_MASK 0x0003
#define CDJ_USB_PID_BUF  0x0001

/* CFIFOCTR: bit 15 BVAL, 14 BCLR, 13 FRDY, 11:0 DTLN (receive data length). */
#define CDJ_USB_BVAL     0x8000
#define CDJ_USB_BCLR     0x4000
#define CDJ_USB_FRDY     0x2000
#define CDJ_USB_DTLN     0x0FFF

/* DCPCFG/PIPECFG bit 4: transfer direction, 1 = transmitting. */
#define CDJ_USB_DIR      0x0010
/* PIPECFG bit 7 SHTNAK: a receiving pipe's PID goes to NAK at end of
 * transfer (manual 31.4.24). */
#define CDJ_USB_SHTNAK   0x0080

/* Which FIFO window a data-port offset falls in, or -1. The ports are 4 bytes
 * wide; the byte lane only narrows the access width. */
static inline int cdj_usb_data_port(hwaddr off)
{
    if (off >= CDJ_USB_CFIFO && off < CDJ_USB_CFIFO + 4) {
        return 0;
    }
    if (off >= CDJ_USB_D0FIFO && off < CDJ_USB_D0FIFO + 4) {
        return 1;
    }
    if (off >= CDJ_USB_D1FIFO && off < CDJ_USB_D1FIFO + 4) {
        return 2;
    }
    /* The R8A66597's own D0FIFO/D1FIFO offsets, aliases of the 0x100/0x120
     * windows. The host driver uses both in one transfer: the DMAC writes the
     * bulk through 0x100 and the CPU writes the tail through 0x18. */
    if (off >= CDJ_USB_D0FIFO_ALT && off < CDJ_USB_D0FIFO_ALT + 4) {
        return 1;
    }
    if (off >= CDJ_USB_D1FIFO_ALT && off < CDJ_USB_D1FIFO_ALT + 4) {
        return 2;
    }
    return -1;
}

/* Which FIFO window a xFIFOSEL / xFIFOCTR register belongs to, or -1. */
static inline int cdj_usb_sel_port(hwaddr off)
{
    switch (off) {
    case CDJ_USB_CFIFOSEL:  return 0;
    case CDJ_USB_D0FIFOSEL: return 1;
    case CDJ_USB_D1FIFOSEL: return 2;
    default:                return -1;
    }
}

static inline int cdj_usb_ctr_port(hwaddr off)
{
    switch (off) {
    case CDJ_USB_CFIFOCTR:  return 0;
    case CDJ_USB_D0FIFOCTR: return 1;
    case CDJ_USB_D1FIFOCTR: return 2;
    default:                return -1;
    }
}

#endif
