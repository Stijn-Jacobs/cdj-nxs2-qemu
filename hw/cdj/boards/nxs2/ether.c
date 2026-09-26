/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj.h"
#include "cdj_getenv.h"
/*
 * SH7724 EtherMAC: E-DMAC at 0x04600000, EtherC at 0x04600100. The E-DMAC
 * walks the firmware's own descriptor rings and passes frames to a QEMU netdev,
 * so Pro DJ Link works between decks. PIR (0x04600120) is the MDIO bit-bang
 * port to the PHY; the firmware talks to a PHY at addresses 0, 1 and 5.
 * Registers are backed because a read-modify-write bit-bang port that reads
 * back zero never converges.
 *
 *   CDJ_ETHER_DEBUG=1      log MDIO frames and the first accesses to each
 *                          register; totals print at exit
 *   CDJ_ETHER_UNBACKED=1   reads return zero
 *   CDJ_ETHER_NO_PHY=1     nothing answers MDIO frames
 *   CDJ_ETHER_NETDEV=<id>  netdev to attach to (default "djlink")
 *   CDJ_ETHER_MAC=<xx:..>  this board's hardware address, see
 *                          cdj_ether_mac_override()
 *   CDJ_ETHER_NOLINK=1     PSR reports the cable unplugged
 *   CDJ_ETHER_NOLCHNG=1    never assert ECSR.LCHNG
 */
#define CDJ_ETHER_BASE 0x04600000
#define CDJ_ETHER_SIZE 0x10000
#define CDJ_ETHER_PIR  0x0120        /* EtherC PIR, absolute 0x04600120 */

#define CDJ_ETHER_EDMR  0x0000
#define CDJ_ETHER_EDTRR 0x0008
#define CDJ_ETHER_EDRRR 0x0010
#define CDJ_ETHER_TDLAR 0x0018
#define CDJ_ETHER_RDLAR 0x0020
#define CDJ_ETHER_EESR  0x0028
#define CDJ_ETHER_EESIPR 0x0030
#define CDJ_ETHER_RMFCR 0x0040
#define CDJ_ETHER_TFUCR 0x0064
#define CDJ_ETHER_RFOCR 0x0068
#define CDJ_ETHER_ECMR  0x0100       /* EtherC, absolute 0x04600100 */
#define CDJ_ETHER_ECSR  0x0110
#define CDJ_ETHER_ECSIPR 0x0118
#define CDJ_ETHER_PSR   0x0128
/* SH7724 manual register list; 0x1A0 is unassigned. */
#define CDJ_ETHER_MAHR  0x01C0
#define CDJ_ETHER_MALR  0x01C8

/* EDMR bit 0 SWR: software reset. The driver polls it until it reads back
 * zero, so the reset completes immediately. */
#define CDJ_EDMR_SWR 0x00000001

/* EDMR bits 5:4 DL give the descriptor stride and bit 6 DE the payload byte
 * order. DE is inverted (0 = swap) and does not apply to the descriptors,
 * which are always in CPU order (manual 47.4.3). */
#define CDJ_EDMR_DE  0x00000040

/* EDTRR TR / EDRRR RR: write 1 to start. The E-DMAC clears TR itself when it
 * reaches a descriptor without TACT (manual 47.4.3(2)). */
#define CDJ_EDTRR_TR 0x00000001
#define CDJ_EDRRR_RR 0x00000001

/* EESR/EESIPR bits, manual 47.4.3(6). ECI is read-only. */
#define CDJ_EESR_TC   0x00200000     /* frame transmit complete          */
#define CDJ_EESR_TDE  0x00100000     /* transmit descriptor empty        */
#define CDJ_EESR_FR   0x00040000     /* frame received                   */
#define CDJ_EESR_RDE  0x00020000     /* receive descriptor empty         */
#define CDJ_EESR_RFOF 0x00010000     /* receive FIFO overflow            */
#define CDJ_EESR_ECI  0x00400000     /* read-only: not cleared by W1C    */

/* ECMR: the two bits that gate the data path, plus promiscuous mode. */
#define CDJ_ECMR_PRM 0x00000001
#define CDJ_ECMR_TE  0x00000020
#define CDJ_ECMR_RE  0x00000040

/* PSR bit 0 LMON follows the PHY's LNKSTA pin: 1 is a live cable. */
#define CDJ_PSR_LMON 0x00000001

/*
 * ECSR/ECSIPR bits, manual 47.4.4(3) and (4). The driver arms LCHNG (link
 * signal change) and ECI and waits for the interrupt before it enables TE/RE
 * in ECMR; it never polls PSR. ECSR bits are write-1-to-clear, and EESR.ECI
 * mirrors "an enabled ECSR source is asserted".
 */
#define CDJ_ECSR_ICD   0x00000001
#define CDJ_ECSR_LCHNG 0x00000004
#define CDJ_ECSR_MPD   0x00000002
#define CDJ_ECSR_PSRTO 0x00000010
#define CDJ_ECSR_W1C   (CDJ_ECSR_ICD | CDJ_ECSR_MPD | CDJ_ECSR_LCHNG | \
                        CDJ_ECSR_PSRTO)

/* Descriptor word 0, manual figures 47.2 and 47.3. TFP/RFP: 01 = end of
 * frame, 10 = start, 11 = whole frame, so bit 28 marks "frame complete". */
#define CDJ_TD0_TACT 0x80000000
#define CDJ_TD0_TDLE 0x40000000
#define CDJ_TD0_TFP1 0x20000000
#define CDJ_TD0_TFP0 0x10000000
#define CDJ_RD0_RACT 0x80000000
#define CDJ_RD0_RDLE 0x40000000
#define CDJ_RD0_RFP1 0x20000000
#define CDJ_RD0_RFP0 0x10000000
#define CDJ_RD0_RFE  0x08000000

/* A frame plus its CRC. The firmware's RBL bounds a receive; this bounds how
 * much is assembled across TX buffers. */
#define CDJ_ETHER_MAXFRAME 1536

/* Bound on a ring walk that never meets an inactive descriptor (BQL held). */
#define CDJ_ETHER_RING_MAX 512

/* PIR bits: MDC clock, MMD data direction, MDO data out, MDI data in. */
#define CDJ_PIR_MDC  0x01
#define CDJ_PIR_MMD  0x02
#define CDJ_PIR_MDO  0x04
#define CDJ_PIR_MDI  0x08

typedef struct {
    MemoryRegion iomem;
    uint32_t reg[CDJ_ETHER_SIZE / 4];
    /* MDIO shift state. Clause-22 frame: preamble, ST(01), OP(2), PHYAD(5),
     * REGAD(5), turnaround, 16 data bits. */
    bool mdc;                        /* previous clock level, for edges */
    bool mdi;                        /* what this end drives back */
    bool in_frame;
    unsigned ones;                   /* consecutive 1s seen, for the preamble */
    uint32_t shift;                  /* command bits clocked in */
    unsigned nbits;
    uint32_t out;                    /* read data being shifted back out */
    unsigned out_bits;
    unsigned wr_reg;                 /* register a write frame is aimed at */
    unsigned wr_phyad;               /* ...and the PHY address it named */
    bool wr_pending;
    uint16_t phy[32];
    /* Per-register access counts, indexed like reg[]. */
    uint32_t nread[CDJ_ETHER_SIZE / 4];
    uint32_t nwrite[CDJ_ETHER_SIZE / 4];
    Notifier exit;
    /* E-DMAC ring cursors. The driver only writes the list start; hardware
     * keeps the position between transmit requests. */
    NICState *nic;
    NICConf conf;
    /* Must not be NULL: QEMU dereferences the NIC's reentrancy guard on the
     * receive path (net/net.c) without a check. This is not a DeviceState, so
     * it owns the guard itself. */
    MemReentrancyGuard reentrancy_guard;
    qemu_irq irq;
    uint32_t tx_cur;
    uint32_t rx_cur;
    uint8_t txbuf[CDJ_ETHER_MAXFRAME];
    unsigned txlen;
    uint64_t ntx, ntx_nonet, nrx, nrx_dropped, nrx_nodesc;
    bool warned_trunc, warned_big;
} CdjEtherState;

/*
 * PHY addresses that answer MDIO. The firmware's link_status(n) at 0x0842840C
 * takes the PHY address; the link-up dispatch at 0x0834577A requires
 * link_status(0) | link_status(1) to be non-zero before link_status(5) picks
 * the ECMR mode at 0x08345936.
 * CDJ_ETHER_PHYADS=<n>[,<n>...] overrides the set.
 */
#define CDJ_ETHER_PHYADS_DEFAULT "0,1,5"

static bool cdj_ether_phy_present(unsigned phyad)
{
    const char *e = getenv("CDJ_ETHER_PHYADS");
    const char *p = e ? e : CDJ_ETHER_PHYADS_DEFAULT;

    while (*p) {
        char *end;
        unsigned long v = strtoul(p, &end, 0);

        if (end != p && v == phyad) {
            return true;
        }
        p = (*end == ',') ? end + 1 : end;
        if (end == p && *p) {
            p++;                     /* junk: step past it rather than spin */
        }
    }
    return false;
}

/* E-DMAC / EtherC register names, manual tables 47.4 and 47.5. */
static const char *cdj_ether_regname(hwaddr off)
{
    switch (off) {
    case 0x000: return "EDMR";
    case 0x008: return "EDTRR";
    case 0x010: return "EDRRR";
    case 0x018: return "TDLAR";
    case 0x020: return "RDLAR";
    case 0x028: return "EESR";
    case 0x030: return "EESIPR";
    case 0x038: return "TRSCER";
    case 0x040: return "RMFCR";
    case 0x048: return "TFTR";
    case 0x050: return "FDR";
    case 0x058: return "RMCR";
    case 0x064: return "TFUCR";
    case 0x068: return "RFOCR";
    case 0x070: return "FCFTR";
    case 0x07c: return "TRIMD";
    case 0x100: return "ECMR";
    case 0x108: return "RFLR";
    case 0x110: return "ECSR";
    case 0x118: return "ECSIPR";
    case 0x120: return "PIR";
    case 0x128: return "PSR";
    case 0x140: return "RDMLR";
    case 0x150: return "IPGR";
    case 0x154: return "APR";
    case 0x158: return "MPR";
    /* Offsets below are from the manual's register list; MAHR/MALR are at
     * 0x1C0/0x1C8. */
    case 0x160: return "RFCF";
    case 0x164: return "TPAUSER";
    case 0x168: return "TPAUSECR";
    case 0x1c0: return "MAHR";
    case 0x1c8: return "MALR";
    case 0x1d0: return "TROCR";
    case 0x1d4: return "CDCR";
    case 0x1d8: return "LCCR";
    case 0x1dc: return "CNDCR";
    case 0x1e4: return "CEFCR";
    case 0x1e8: return "FRECR";
    case 0x1ec: return "TSFRCR";
    case 0x1f0: return "TLFRCR";
    case 0x1f4: return "RFCR";
    case 0x1f8: return "MAFCR";
    default:    return NULL;
    }
}

/* Log the first few accesses to each register and count the rest. */
#define CDJ_ETHER_LOG_EACH 6

#define MII_BMCR    0
#define MII_BMSR    1
#define MII_ANAR    4
#define MII_ANLPAR  5

/*
 * Lead-in bits driven before a read's 16 data bits. The driver has already
 * clocked past the turnaround when it starts sampling, so the default is 0;
 * with 2 it reads every register shifted right by two.
 * CDJ_ETHER_MDIO_TA=<n> overrides it.
 */
static unsigned cdj_ether_mdio_ta(void)
{
    const char *e = getenv("CDJ_ETHER_MDIO_TA");

    return e ? (unsigned)strtoul(e, NULL, 0) : 0;
}

static uint16_t cdj_ether_phy_read(CdjEtherState *s, unsigned reg)
{
    switch (reg) {
    case MII_BMSR:
        /* Link up and autoneg complete, or the driver waits forever:
         * 0x7800 capabilities, 0x0020 autoneg done, 0x0008 autoneg able,
         * 0x0004 link up, 0x0001 extended capability.
         * CDJ_ETHER_BMSR=<val> overrides it. */
        {
            const char *e = getenv("CDJ_ETHER_BMSR");

            return e ? (uint16_t)strtoul(e, NULL, 0) : 0x782D;
        }
    case MII_ANLPAR:
        return 0x45E1;               /* link partner: 100/10, ack */
    default:
        return s->phy[reg & 0x1f];
    }
}

static void cdj_ether_mdio_bit(CdjEtherState *s, bool bit)
{
    if (!s->in_frame) {
        /*
         * The preamble is nominally 32 ones, but this driver does not always
         * send a full one, so "some ones, then a zero" marks ST. Only preamble
         * and ST can be on the line here (MMD=1, no frame in progress).
         */
        if (bit) {
            s->ones++;
        } else if (s->ones >= 2) {
            s->in_frame = true;
            s->shift = 0;
            s->nbits = 0;
            s->ones = 0;
        } else {
            s->ones = 0;
        }
        return;
    }

    s->shift = (s->shift << 1) | bit;
    if (++s->nbits < 13) {           /* ST[0] + OP(2) + PHYAD(5) + REGAD(5) */
        return;
    }
    s->in_frame = false;
    {
        unsigned op = (s->shift >> 10) & 0x3;
        unsigned phyad = (s->shift >> 5) & 0x1f;
        unsigned regad = s->shift & 0x1f;

        if (getenv("CDJ_ETHER_DEBUG")) {
            if (op == 0x2) {
                info_report("ether: MDIO read  phy %u reg %-2u -> 0x%04x",
                            phyad, regad, cdj_ether_phy_read(s, regad));
            } else {
                info_report("ether: MDIO write phy %u reg %-2u", phyad, regad);
            }
        }
        /* CDJ_ETHER_NO_PHY=1 leaves the line idle. */
        if (!cdj_ether_phy_present(phyad) || getenv("CDJ_ETHER_NO_PHY")) {
            return;                  /* no PHY there: the line stays idle */
        }
        if (op == 0x2) {
            s->out = cdj_ether_phy_read(s, regad);
            /* 16 data bits preceded by CDJ_ETHER_MDIO_TA lead-in bits. */
            s->out_bits = 16 + cdj_ether_mdio_ta();
        } else {
            s->wr_reg = regad;
            s->wr_phyad = phyad;
            s->wr_pending = true;
            s->out = 0;
            s->out_bits = 0;
        }
    }
}


/*
 * E-DMAC descriptor engine. Descriptors (manual figures 47.2 and 47.3) are
 * three longwords in a 16/32/64-byte slot: status/control, lengths, buffer
 * address. Hardware owns a descriptor while TACT/RACT is set and returns it by
 * clearing that bit.
 */

/* Descriptors are in CPU byte order regardless of EDMR.DE, and MAIN is
 * little-endian. */
static uint32_t cdj_ether_ld(hwaddr addr)
{
    uint32_t v;

    cpu_physical_memory_read(A7ADDR(addr), &v, sizeof(v));
    return le32_to_cpu(v);
}

static void cdj_ether_st(hwaddr addr, uint32_t val)
{
    uint32_t v = cpu_to_le32(val);

    cpu_physical_memory_write(A7ADDR(addr), &v, sizeof(v));
}

/* EDMR DL[1:0]: 00 = 16 bytes, 01 = 32, 10 = 64; 11 is prohibited. */
static unsigned cdj_ether_desc_len(CdjEtherState *s)
{
    switch ((s->reg[CDJ_ETHER_EDMR / 4] >> 4) & 0x3) {
    case 1:  return 32;
    case 2:  return 64;
    default: return 16;
    }
}

/* DE = 0 asks the E-DMAC to byte-swap payload longwords. */
static void cdj_ether_swap_payload(CdjEtherState *s, uint8_t *p, unsigned len)
{
    unsigned i;

    if (s->reg[CDJ_ETHER_EDMR / 4] & CDJ_EDMR_DE) {
        return;
    }
    for (i = 0; i + 4 <= len; i += 4) {
        uint8_t t;

        t = p[i]; p[i] = p[i + 3]; p[i + 3] = t;
        t = p[i + 1]; p[i + 1] = p[i + 2]; p[i + 2] = t;
    }
}

static void cdj_ether_update_irq(CdjEtherState *s)
{
    uint32_t active;

    /* ECI is a read-only mirror of "an enabled EtherC source is asserted",
     * so it is recomputed rather than latched. */
    if (s->reg[CDJ_ETHER_ECSR / 4] & s->reg[CDJ_ETHER_ECSIPR / 4] &
        CDJ_ECSR_W1C) {
        s->reg[CDJ_ETHER_EESR / 4] |= CDJ_EESR_ECI;
    } else {
        s->reg[CDJ_ETHER_EESR / 4] &= ~(uint32_t)CDJ_EESR_ECI;
    }

    active = s->reg[CDJ_ETHER_EESR / 4] & s->reg[CDJ_ETHER_EESIPR / 4];
    if (s->irq) {
        qemu_set_irq(s->irq, active ? 1 : 0);
    }
}

/* Latch LCHNG once, as a cable present at power on looks to hardware. Latched
 * rather than level-driven: the driver clears it by writing 1, and
 * re-asserting it would storm the interrupt. */
static void cdj_ether_link_change(CdjEtherState *s)
{
    if (getenv("CDJ_ETHER_NOLCHNG") || getenv("CDJ_ETHER_NOLINK")) {
        return;
    }
    s->reg[CDJ_ETHER_ECSR / 4] |= CDJ_ECSR_LCHNG;
    cdj_ether_update_irq(s);
}

static void cdj_ether_raise(CdjEtherState *s, uint32_t bits)
{
    s->reg[CDJ_ETHER_EESR / 4] |= bits;
    cdj_ether_update_irq(s);
}

/* Defined below, beside the filter that uses the same address. */
static const uint8_t *cdj_ether_mac_override(void);

/*
 * CDJ_ETHER_LINKLOG=1: one stderr line per Pro DJ Link frame (UDP 50000-50002)
 * sent or received, with both the virtual and the host clock:
 *   ETHLINK tx|rx vt=<virtual s> ht=<host s> ip=.<last octet> port=<n> type=0x<t> len=<n>
 */
static void cdj_ether_linklog(const char *dir, const uint8_t *buf, size_t size)
{
    static int on = -1;
    unsigned ihl, sport, dport;
    const uint8_t *udp;

    if (on < 0) {
        const char *e = getenv("CDJ_ETHER_LINKLOG");
        on = e && *e && strcmp(e, "0") != 0;
    }
    if (!on || size < 14 + 20 || buf[12] != 0x08 || buf[13] != 0x00) {
        return;
    }
    ihl = (buf[14] & 0x0f) * 4;
    if (buf[14 + 9] != 17 || size < 14 + ihl + 8 + 11) {
        return;
    }
    udp = buf + 14 + ihl;
    sport = (udp[0] << 8) | udp[1];
    dport = (udp[2] << 8) | udp[3];
    if (dport < 50000 || dport > 50002) {
        return;
    }
    fprintf(stderr, "ETHLINK %s vt=%.4f ht=%.4f ip=.%u port=%u/%u type=0x%02x "
            "len=%zu\n", dir,
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e9,
            qemu_clock_get_ns(QEMU_CLOCK_REALTIME) / 1e9,
            buf[14 + 15], sport, dport, udp[8 + 10], size - 14 - ihl - 8);
}

/* Walk the transmit ring until a descriptor without TACT, then clear TR. */
static void cdj_ether_tx(CdjEtherState *s)
{
    unsigned dlen = cdj_ether_desc_len(s);
    unsigned guard;

    /* No !s->nic check: without a netdev the ring must still be walked and
     * TACT cleared, or the driver hangs on its first transmit. The frames are
     * dropped. */
    if (!(s->reg[CDJ_ETHER_EDTRR / 4] & CDJ_EDTRR_TR)) {
        return;
    }

    for (guard = 0; guard < CDJ_ETHER_RING_MAX; guard++) {
        hwaddr d = s->tx_cur;
        uint32_t td0 = cdj_ether_ld(d);
        uint32_t td1 = cdj_ether_ld(d + 4);
        uint32_t td2 = cdj_ether_ld(d + 8);
        unsigned len = td1 >> 16;

        if (!(td0 & CDJ_TD0_TACT)) {
            s->reg[CDJ_ETHER_EDTRR / 4] &= ~(uint32_t)CDJ_EDTRR_TR;
            return;
        }

        if (len > sizeof(s->txbuf) - s->txlen) {
            len = sizeof(s->txbuf) - s->txlen;
            if (!s->warned_trunc) {
                s->warned_trunc = true;
                warn_report("ether: transmit buffer longer than %zu bytes -- "
                            "truncated (this frame and any later one)",
                            sizeof(s->txbuf));
            }
        }
        if (len) {
            cpu_physical_memory_read(A7ADDR(td2), s->txbuf + s->txlen, len);
            cdj_ether_swap_payload(s, s->txbuf + s->txlen, len);
            s->txlen += len;
        }

        /* TFP0 is set for "end of frame" (01) and "whole frame" (11). */
        if (td0 & CDJ_TD0_TFP0) {
            const uint8_t *over = cdj_ether_mac_override();

            /* Bytes 6..11 are the source address; stamp it once the frame
             * is assembled. */
            if (over && s->txlen >= 12) {
                memcpy(s->txbuf + 6, over, 6);
            }
            if (s->nic) {
                cdj_ether_linklog("tx", s->txbuf, s->txlen);
                qemu_send_packet(qemu_get_queue(s->nic), s->txbuf, s->txlen);
                s->ntx++;
            } else {
                s->ntx_nonet++;
            }
            s->txlen = 0;
            cdj_ether_raise(s, CDJ_EESR_TC);
        }

        cdj_ether_st(d, td0 & ~(uint32_t)CDJ_TD0_TACT);   /* hand it back */
        s->tx_cur = (td0 & CDJ_TD0_TDLE) ? s->reg[CDJ_ETHER_TDLAR / 4]
                                         : (uint32_t)(d + dlen);
    }
    warn_report("ether: transmit ring walked %u descriptors without an "
                "inactive one -- stopping to avoid spinning under the BQL",
                CDJ_ETHER_RING_MAX);
    s->reg[CDJ_ETHER_EDTRR / 4] &= ~(uint32_t)CDJ_EDTRR_TR;
}

/*
 * CDJ_ETHER_MAC=<xx:xx:xx:xx:xx:xx>: this board's hardware address.
 * The firmware reads its MAC from board storage, which is blank in the flash
 * dump, so every deck calls itself 00:00:00:00:00:01 and two decks on one
 * segment would collide. The override is applied in the transmit, filter and
 * MAHR/MALR read paths. Unset, the firmware's own address is used.
 */
static const uint8_t *cdj_ether_mac_override(void)
{
    static uint8_t mac[6];
    static int state;                /* 0 unknown, 1 set, -1 none */

    if (!state) {
        const char *e = getenv("CDJ_ETHER_MAC");
        unsigned b[6];

        if (e && sscanf(e, "%x:%x:%x:%x:%x:%x",
                        &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6) {
            for (unsigned i = 0; i < 6; i++) {
                mac[i] = (uint8_t)b[i];
            }
            state = 1;
            if (mac[0] & 0x01) {
                warn_report("ether: CDJ_ETHER_MAC %s has the multicast bit "
                            "set -- frames from it will be filtered oddly", e);
            }
        } else {
            if (e && *e) {
                warn_report("ether: CDJ_ETHER_MAC='%s' is not six hex octets "
                            "-- ignored", e);
            }
            state = -1;
        }
    }
    return state > 0 ? mac : NULL;
}

/* MAHR/MALR are read live: the driver programs them after reset.
 * CDJ_ETHER_MAC, when set, is also accepted. */
static bool cdj_ether_accept(CdjEtherState *s, const uint8_t *buf, size_t size)
{
    uint32_t mahr = s->reg[CDJ_ETHER_MAHR / 4];
    uint32_t malr = s->reg[CDJ_ETHER_MALR / 4];
    const uint8_t *over = cdj_ether_mac_override();
    uint8_t mac[6];

    if (size < 14) {
        return false;
    }
    if (s->reg[CDJ_ETHER_ECMR / 4] & CDJ_ECMR_PRM) {
        return true;
    }
    /*
     * Drop our own frames. The socket netdev is a shared bus, so every
     * multicast comes back to its sender; with CDJ_ETHER_MAC set the firmware
     * (which keeps its own address at 0x0A35F754) does not recognise its own
     * Pro DJ Link claim and restarts the claim forever.
     */
    mac[0] = mahr >> 24; mac[1] = mahr >> 16; mac[2] = mahr >> 8; mac[3] = mahr;
    mac[4] = malr >> 8;  mac[5] = malr;
    if (memcmp(buf + 6, over ? over : mac, 6) == 0) {
        return false;
    }
    if (buf[0] & 0x01) {
        return true;                 /* broadcast and multicast, Pro DJ Link
                                      * announces to the subnet broadcast */
    }
    /*
     * With the override set the board has two addresses: the firmware still
     * answers ARP with MAHR/MALR, so peers send unicast to that one. Accept
     * both.
     */
    if (over && memcmp(buf, over, 6) == 0) {
        return true;
    }
    return memcmp(buf, mac, sizeof(mac)) == 0;
}

/* Gated on RR, not RE, so frames queued before the receiver is enabled are
 * not lost. */
static bool cdj_ether_can_receive(NetClientState *nc)
{
    CdjEtherState *s = qemu_get_nic_opaque(nc);

    return (s->reg[CDJ_ETHER_EDRRR / 4] & CDJ_EDRRR_RR) != 0;
}

static ssize_t cdj_ether_receive(NetClientState *nc, const uint8_t *buf,
                                 size_t size)
{
    CdjEtherState *s = qemu_get_nic_opaque(nc);
    unsigned dlen = cdj_ether_desc_len(s);
    hwaddr d = s->rx_cur;
    uint32_t rd0, rd1, rd2, rbl;

    if (!(s->reg[CDJ_ETHER_ECMR / 4] & CDJ_ECMR_RE) ||
        !cdj_ether_accept(s, buf, size)) {
        s->nrx_dropped++;
        return size;                 /* consumed: not ours, do not re-queue */
    }

    rd0 = cdj_ether_ld(d);
    rd1 = cdj_ether_ld(d + 4);
    rd2 = cdj_ether_ld(d + 8);

    /* No active descriptor: the frame is lost, as on hardware. */
    if (!(rd0 & CDJ_RD0_RACT)) {
        s->nrx_nodesc++;
        /* RDE only; the FIFO has not overflowed. */
        cdj_ether_raise(s, CDJ_EESR_RDE);
        return size;
    }

    rbl = rd1 >> 16;
    if (size > rbl || size > CDJ_ETHER_MAXFRAME) {
        /* Multi-descriptor receive is not modelled; nothing on a Pro DJ Link
         * segment needs it. No EESR bit is raised, since the modelled
         * hardware has not failed. */
        if (!s->warned_big) {
            s->warned_big = true;
            warn_report("ether: %zu-byte frame does not fit the %u-byte "
                        "receive buffer -- dropped (multi-descriptor receive "
                        "is not modelled; this warns once)", size, rbl);
        }
        s->nrx_dropped++;
        return size;
    }

    /* The frame is const, so a DE = 0 swap goes through a scratch copy.
     * size is bounded by rbl and CDJ_ETHER_MAXFRAME above. */
    if (s->reg[CDJ_ETHER_EDMR / 4] & CDJ_EDMR_DE) {
        cpu_physical_memory_write(A7ADDR(rd2), buf, size);
    } else {
        uint8_t tmp[CDJ_ETHER_MAXFRAME];

        memcpy(tmp, buf, size);
        cdj_ether_swap_payload(s, tmp, size);
        cpu_physical_memory_write(A7ADDR(rd2), tmp, size);
    }

    /* RFL is the low half of RD1 (RBL in the high half is preserved).
     * RFP = 11: this buffer holds the whole frame. */
    cdj_ether_st(d + 4, (rbl << 16) | (uint32_t)(size & 0xffff));
    rd0 &= ~(uint32_t)(CDJ_RD0_RACT | CDJ_RD0_RFE | 0x0000ffff);
    rd0 |= CDJ_RD0_RFP1 | CDJ_RD0_RFP0;
    cdj_ether_st(d, rd0);

    s->rx_cur = (rd0 & CDJ_RD0_RDLE) ? s->reg[CDJ_ETHER_RDLAR / 4]
                                     : (uint32_t)(d + dlen);
    s->nrx++;
    cdj_ether_linklog("rx", buf, size);
    cdj_ether_raise(s, CDJ_EESR_FR);
    return size;
}

static NetClientInfo cdj_ether_net_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = cdj_ether_can_receive,
    .receive = cdj_ether_receive,
};

/* CDJ_ETHER_UNBACKED=1 makes every register read as zero. */
static bool cdj_ether_backed(void)
{
    return !getenv("CDJ_ETHER_UNBACKED");
}

/* Trace one non-PIR register access; the count exposes polling loops. */
static void cdj_ether_trace(CdjEtherState *s, hwaddr off, uint32_t val,
                            bool write)
{
    const char *name;
    uint32_t n;

    if (off == CDJ_ETHER_PIR) {
        return;                      /* the MDIO port drowns out everything */
    }
    n = ++(write ? s->nwrite : s->nread)[off / 4];
    if (n > CDJ_ETHER_LOG_EACH || !getenv("CDJ_ETHER_DEBUG")) {
        return;
    }
    name = cdj_ether_regname(off);
    info_report("ether: %s %-7s (+0x%03x) = 0x%08x  #%u",
                write ? "write" : "read ", name ? name : "?",
                (unsigned)off, val, n);
}

static uint64_t cdj_ether_read(void *opaque, hwaddr off, unsigned size)
{
    CdjEtherState *s = opaque;
    uint32_t v;

    if (!cdj_ether_backed()) {
        cdj_ether_trace(s, off, 0, false);
        return 0;
    }
    v = s->reg[off / 4];
    /* CDJ_ETHER_MAC also answers MAHR/MALR, so the firmware's idea of its own
     * address (used in Pro DJ Link announces and tie-breaks) is per deck. */
    {
        const uint8_t *over = cdj_ether_mac_override();

        if (over) {
            if (off == CDJ_ETHER_MAHR) {
                v = ((uint32_t)over[0] << 24) | ((uint32_t)over[1] << 16) |
                    ((uint32_t)over[2] << 8) | over[3];
            } else if (off == CDJ_ETHER_MALR) {
                v = ((uint32_t)over[4] << 8) | over[5];
            }
        }
    }
    if (off == CDJ_ETHER_PIR) {
        v = (v & ~(uint32_t)CDJ_PIR_MDI) | (s->mdi ? CDJ_PIR_MDI : 0);
    }
    /* PSR is the link-status pin. The firmware logs "ETHER linkdown" and
     * refuses to send while it reads 0. */
    if (off == CDJ_ETHER_PSR) {
        v = getenv("CDJ_ETHER_NOLINK") ? 0 : CDJ_PSR_LMON;
    }
    cdj_ether_trace(s, off, v, false);
    return v;
}

/* Software reset. TDLAR, RDLAR, RMFCR, TFUCR and RFOCR survive it
 * (manual). */
static void cdj_ether_swr(CdjEtherState *s)
{
    static const hwaddr keep[] = {
        CDJ_ETHER_TDLAR, CDJ_ETHER_RDLAR, CDJ_ETHER_RMFCR,
        CDJ_ETHER_TFUCR, CDJ_ETHER_RFOCR,
    };
    uint32_t saved[ARRAY_SIZE(keep)];
    unsigned i;

    for (i = 0; i < ARRAY_SIZE(keep); i++) {
        saved[i] = s->reg[keep[i] / 4];
    }
    memset(s->reg, 0, sizeof(s->reg));
    for (i = 0; i < ARRAY_SIZE(keep); i++) {
        s->reg[keep[i] / 4] = saved[i];
    }
}

static void cdj_ether_write(void *opaque, hwaddr off, uint64_t val,
                            unsigned size)
{
    CdjEtherState *s = opaque;

    if (off == CDJ_ETHER_EDMR && (val & CDJ_EDMR_SWR)) {
        cdj_ether_swr(s);
        val &= ~(uint64_t)CDJ_EDMR_SWR;      /* the reset has already finished */
        /* A reset returns the rings to their list starts, which survive it. */
        s->tx_cur = s->reg[CDJ_ETHER_TDLAR / 4];
        s->rx_cur = s->reg[CDJ_ETHER_RDLAR / 4];
        s->txlen = 0;
        /* The reset wiped ECSR, so re-latch the cable; it is delivered once
         * the driver arms ECSIPR. */
        cdj_ether_link_change(s);
        cdj_ether_update_irq(s);
    }

    /* EESR is write-1-to-clear; ECI is read-only. */
    if (off == CDJ_ETHER_EESR) {
        s->reg[off / 4] &= ~(uint32_t)(val & ~(uint64_t)CDJ_EESR_ECI);
        cdj_ether_trace(s, off, (uint32_t)val, true);
        cdj_ether_update_irq(s);
        return;
    }

    /* ECSR is write-1-to-clear; clearing it can drop EESR.ECI, so update
     * the IRQ. */
    if (off == CDJ_ETHER_ECSR) {
        s->reg[off / 4] &= ~(uint32_t)(val & CDJ_ECSR_W1C);
        cdj_ether_trace(s, off, (uint32_t)val, true);
        cdj_ether_update_irq(s);
        return;
    }

    /* The list start registers reposition the ring cursor; EDTRR/EDRRR start
     * from wherever it already is. */
    if (off == CDJ_ETHER_TDLAR) {
        s->tx_cur = (uint32_t)val;
    } else if (off == CDJ_ETHER_RDLAR) {
        s->rx_cur = (uint32_t)val;
    }

    if (off == CDJ_ETHER_PIR) {
        bool mdc = (val & CDJ_PIR_MDC) != 0;

        if (mdc && !s->mdc) {                    /* rising edge */
            if (val & CDJ_PIR_MMD) {
                bool bit = (val & CDJ_PIR_MDO) != 0;

                if (s->wr_pending) {
                    /* Write data phase, MSB first. The two turnaround bits
                     * land in the discarded high bits. */
                    s->out = (s->out << 1) | bit;
                    if (++s->out_bits == 18) {
                        uint16_t v = s->out & 0xffff;

                        /* BMCR RESET (15) and RESTART_AN (9) self-clear. */
                        if (s->wr_reg == MII_BMCR) {
                            v &= ~0x8200;
                        }
                        /* BMCR is read-modify-write, so the written value
                         * shows what the driver made of our read. */
                        if (getenv("CDJ_ETHER_DEBUG")) {
                            info_report("ether: MDIO wrote phy %u reg %-2u "
                                        "<- 0x%04x (we had returned 0x%04x)",
                                        s->wr_phyad, s->wr_reg, v,
                                        cdj_ether_phy_read(s, s->wr_reg));
                        }
                        s->phy[s->wr_reg] = v;
                        s->wr_pending = false;
                        s->out = s->out_bits = 0;
                    }
                } else {
                    cdj_ether_mdio_bit(s, bit);
                }
            } else if (s->out_bits) {
                /* A read frame is 18 clocks: 2 turnaround, then 16 data bits
                 * MSB first. */
                s->mdi = s->out_bits > 16
                         ? false
                         : (s->out & (1u << (s->out_bits - 1))) != 0;
                s->out_bits--;
            } else {
                s->mdi = false;
            }
        }
        s->mdc = mdc;
        val &= ~(uint64_t)CDJ_PIR_MDI;           /* MDI is an input */
    }
    cdj_ether_trace(s, off, (uint32_t)val, true);
    s->reg[off / 4] = (uint32_t)val;

    if (off == CDJ_ETHER_EDTRR && (val & CDJ_EDTRR_TR)) {
        cdj_ether_tx(s);
    } else if (off == CDJ_ETHER_EDRRR && (val & CDJ_EDRRR_RR)) {
        /* Flush frames queued while RR was clear. */
        if (s->nic) {
            qemu_flush_queued_packets(qemu_get_queue(s->nic));
        }
    } else if (off == CDJ_ETHER_EESIPR || off == CDJ_ETHER_ECSIPR) {
        /* Arming can make an already-latched source (LCHNG) deliverable. */
        cdj_ether_update_irq(s);
    }
}

static const MemoryRegionOps cdj_ether_ops = {
    .read = cdj_ether_read,
    .write = cdj_ether_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

/* Per-register totals: a register read thousands of times is being polled. */
static void cdj_ether_dump(Notifier *n, void *unused)
{
    CdjEtherState *s = container_of(n, CdjEtherState, exit);
    unsigned i;

    info_report("ether: tx=%" PRIu64 " rx=%" PRIu64 " dropped=%" PRIu64
                " no-descriptor=%" PRIu64 " discarded-no-netdev=%" PRIu64 "%s",
                s->ntx, s->nrx, s->nrx_dropped, s->nrx_nodesc, s->ntx_nonet,
                s->nic ? "" : "  (no netdev attached)");
    for (i = 0; i < CDJ_ETHER_SIZE / 4; i++) {
        const char *name;

        if (!s->nread[i] && !s->nwrite[i]) {
            continue;
        }
        name = cdj_ether_regname(4 * i);
        info_report("ether: %-7s (+0x%03x) reads=%u writes=%u = 0x%08x",
                    name ? name : "?", 4 * i, s->nread[i], s->nwrite[i],
                    s->reg[i]);
    }
}

/* The MAC attaches to a named netdev, e.g. for two decks on one segment:
 *
 *   -netdev socket,id=djlink,mcast=230.0.0.1:50000
 *
 * Without it the link comes up but frames go nowhere. */
void cdj_ether_init(MemoryRegion *sysmem, qemu_irq irq)
{
    CdjEtherState *s = g_new0(CdjEtherState, 1);
    const char *id = getenv("CDJ_ETHER_NETDEV");
    NetClientState *peer;

    memory_region_init_io(&s->iomem, NULL, &cdj_ether_ops, s, "sh7724.ether",
                          CDJ_ETHER_SIZE);
    memory_region_add_subregion_overlap(sysmem, CDJ_ETHER_BASE, &s->iomem, 1);
    s->exit.notify = cdj_ether_dump;
    qemu_add_exit_notifier(&s->exit);
    s->irq = irq;
    cdj_ether_link_change(s);        /* the cable is in at power on */

    peer = qemu_find_netdev(id && *id ? id : "djlink");
    if (!peer) {
        info_report("cdj2000nxs2: no '%s' netdev -- the EtherMAC will bring "
                    "its link up but no frame will leave the machine",
                    id && *id ? id : "djlink");
        return;
    }
    s->conf.peers.ncs[0] = peer;
    s->conf.peers.queues = 1;
    s->nic = qemu_new_nic(&cdj_ether_net_info, &s->conf, "cdj.ether",
                          "ether", &s->reentrancy_guard, s);
    info_report("cdj2000nxs2: EtherMAC attached to netdev '%s'",
                id && *id ? id : "djlink");
}

