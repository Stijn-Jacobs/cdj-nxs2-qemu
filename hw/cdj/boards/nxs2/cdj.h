/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Shared declarations for the CDJ-2000NXS2 board model. What every board
 * shares is in common/cdj_common.h. */
#ifndef CDJ_H
#define CDJ_H
#include "qemu/osdep.h"
#include "cdj_common.h"
#include "qemu/units.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qemu/thread.h"
#include "qemu/main-loop.h"
#include "qapi/error.h"
#include "cpu.h"
#include "hw/sysbus.h"
#include "hw/boards.h"
#include "hw/loader.h"
#include "hw/qdev-properties.h"
#include "hw/misc/unimp.h"
#include "hw/block/flash.h"
#include "chardev/char-fe.h"
/* QEMU 9.1 still uses the sysemu/ include prefix. */
#include "sysemu/blockdev.h"
#include "sysemu/block-backend.h"
#include "hw/sh4/sh.h"
#include "hw/sh4/sh_intc.h"
#include "hw/timer/tmu012.h"
#include "hw/irq.h"
#include "hw/usb.h"
#include "net/net.h"
#include "exec/cdj-pcring.h"
#include "sysemu/reset.h"
#include "sysemu/runstate.h"
#include "qemu/notify.h"
#include "sysemu/sysemu.h"
#include "exec/address-spaces.h"
#include "ui/console.h"
#include "ui/surface.h"
#include "ui/vgafont.h"
#include "cdj_panelkeys.h"
#include "c6x/c66x.h"
#include "c6x/soc_c6655.h"
#ifdef _WIN32
#include "tcg/tcg.h"
#include "exec/translation-block.h"
#include <psapi.h>
#endif

/* USB 2.0 host module; the DMAC needs it to recognise its FIFO ports. */
#define CDJ_USB_BASE 0x04D80000
#define CDJ_USB_SIZE 0x1000

/* NOR flash on CS0, 8 MB (127 x 64 KB + 8 x 8 KB per the firmware's sector
 * tables). Erased cells read 0xFF, which the settings-log scan relies on. */
#define CDJ_FLASH_PHYS      0x00000000
#define CDJ_FLASH_SIZE      (8 * MiB)

/* DRAM, 0x08000000-0x18000000. The image lands at the base; the bootloader
 * stages at 0x1785D000 and 0x17FFD000. */
#define CDJ_DRAM_PHYS       0x08000000
#define CDJ_DRAM_SIZE       (256 * MiB)
#define CDJ_AREA4_PHYS      0x10000000      /* CS4, opt-in via CDJ_AREA4 */
#define CDJ_AREA4_SIZE      (16 * MiB)
#define CDJ_AREA6_PHYS      0x18000000      /* CS6, opt-in via CDJ_AREA6 */
#define CDJ_AREA6_SIZE      (16 * MiB)

#include "audio/audio.h"

/* Entry is image offset 0x800 (from the decompressor stub's literal pool).
 * With VBR at the image base: +0x100 general exception, +0x400 TLB miss,
 * +0x600 interrupt, +0x800 _start. */
#define CDJ_FW_ENTRY        0xA8000800   /* P2 | (DRAM base + 0x800) */
#define CDJ_INIT_SP         0xB8000000   /* value the real bootloader loads */

/* The RTOS restores SR from this global, not the stack (0x085129E0 and
 * 0x085129EC restore it; only 0x085129D0 writes it). A restore before the
 * first write would load 0, drop to user mode and fault on the next P2
 * access, so the board seeds it. The firmware overwrites it later. */
#define CDJ_SR_SAVE_SLOT    0x0AB87D8C
#define CDJ_SR_SEED         0x400000F0   /* MD=1, BL=0, RB=0, IMASK=0xF */

#define CDJ_SCIF0_ADDR      0xFFE00000
#define CDJ_SCIF1_ADDR      0xFFE10000
#define CDJ_SCIF2_ADDR      0xFFE20000

#define CDJ_DMA1_BASE       0xFDC08000
#define CDJ_DMA1_SIZE       0x2000
#define CDJ_DMA1_CH0        0x20        /* channel 0 registers start here */
#define CDJ_DMA1_DMAOR      0x60
#define CDJ_DMA1_CHANS      6
#define CDJ_MSIOF1_SITFDR   0xA4C50050
#define CDJ_MSIOF1_SIRFDR   0xA4C50060
#define CDJ_MSIOF0_SITFDR   0xA4C40050
#define CDJ_MSIOF0_SIRFDR   0xA4C40060

typedef struct CdjDma1State {
    MemoryRegion iomem;
    uint32_t reg[CDJ_DMA1_SIZE / 4];
    bool pending[CDJ_DMA1_CHANS];
    /* one SAR/DAR report per channel, see cdj_dma1_run() */
    bool sar_reported[CDJ_DMA1_CHANS];
    bool copy_reported[CDJ_DMA1_CHANS];
    FILE *cap[CDJ_DMA1_CHANS];   /* CDJ_CAPTURE sinks, opened lazily */
    QEMUTimer *retry;
    qemu_irq dei[CDJ_DMA1_CHANS];
    unsigned dei_raised[CDJ_DMA1_CHANS];
    /* Per-channel arm/refusal counters, printed at exit. */
    unsigned armed[CDJ_DMA1_CHANS];      /* CHCR written with DE=1, TE=0 */
    unsigned armed_te[CDJ_DMA1_CHANS];   /* CHCR written with DE=1, TE=1 */
    unsigned ran[CDJ_DMA1_CHANS];        /* cdj_dma1_run past its gate */
    unsigned no_de[CDJ_DMA1_CHANS];      /* refused: DE clear */
    unsigned no_te[CDJ_DMA1_CHANS];      /* refused: TE still set */
    unsigned no_dmaor[CDJ_DMA1_CHANS];   /* refused: master enable off */
    unsigned moved0[CDJ_DMA1_CHANS];     /* ran, moved nothing, raised nothing */
    int64_t last_run_ms[CDJ_DMA1_CHANS];
    int64_t last_arm_ms[CDJ_DMA1_CHANS];
    /* Runs per second of wall clock, and the deferred-transmit counters. */
    unsigned run_bucket[CDJ_DMA1_CHANS][60];   /* 1 s each, 0-60 s */
    unsigned defer_hit[CDJ_DMA1_CHANS];        /* arm swallowed by txdefer */
    unsigned defer_start[CDJ_DMA1_CHANS];      /* transmits that deferred */
    unsigned defer_dropped[CDJ_DMA1_CHANS];    /* tails given up on */
    unsigned defer_dropped_bytes[CDJ_DMA1_CHANS];
    unsigned queued[CDJ_DMA1_CHANS];           /* transfers completed to queue */
    uint64_t drained;                          /* bytes the timer pushed out */
    int64_t defer_since_ms;                    /* when the current one began */
    GByteArray *txhold;      /* frame tail the socket would not take yet */
    bool txdefer;            /* a transmit is waiting on that tail to drain */
    unsigned txdefer_ch;
    uint32_t txdefer_sar;    /* SAR as it stood after the copy */
    /* Transmit pacing, CDJ_SPILINK_PACE_US (default 0, off). A whole
     * transfer is held before it starts, never split mid-frame. */
    int64_t pace_next;                         /* earliest next transmit, ns */
    unsigned paced[CDJ_DMA1_CHANS];            /* transfers held by pacing */
    Notifier exit;
} CdjDma1State;

typedef struct CdjDspEng {
    bool on;
    unsigned rate;              /* bytes of compressed audio per second      */
    uint64_t boot_bytes;        /* ch3 traffic sourced from MAIN's flash     */
    uint64_t audio_bytes;       /* ch3 traffic sourced from RAM: the track   */
    int64_t first_audio_ms;     /* when the first audio byte arrived         */
    int64_t last_report_ms;
    unsigned ships;
    uint32_t pos_addr;          /* CDJ_DSP_ENGINE_POS: where MAIN reads it  */
    unsigned hz;                /* publish rate                             */
    bool raw;                   /* CDJ_DSP_ENGINE_RAW: one bare address     */
    uint64_t published;
    /* CDJ_DSP_ENGINE_GATE -- only decode while the deck says it is playing. */
    bool gate_on;
    uint32_t gate_addr, gate_val, gate_mask;
    int64_t play_ms;            /* time accrued with the gate PASSING       */
    int64_t gate_last_ms;       /* when that was last sampled               */
    int64_t last_pub_report_ms;
    uint64_t gate_pass, gate_fail;
    /* CDJ_CUEKEY: cue point and hold, see cdj_dsp_engine_cue(). */
    bool cue_parsed, cue_on, cued;
    bool pos_init;              /* the pitch-scaled position integrator     */
    double pos_acc_ms;
    int64_t pos_base_ms;
    unsigned cue_off, cue_mask, play_off, play_mask;
    int64_t cue_ms;
    uint64_t cue_presses, play_presses;
    /* CDJ_JOGDRIVE: the platter, read from MAIN's decoded jog slots, so any
     * panel input source drives it. */
    bool jog_parsed, jog_on;
    uint32_t jog_mov_addr, jog_fwd_addr, jog_touch_addr;
    int32_t jog_bend_pct;       /* extra % of rate while the platter turns   */
    int32_t jog_scrub_ms;       /* ms of track per 20 ms while touched       */
    double jog_off_ms;          /* the platter's own contribution, signed    */
    int64_t jog_last_ms;
    int jog_last_state;         /* bit 0 mov, bit 1 fwd, bit 2 touch         */
    bool jog_held;              /* a hand is on the platter: hold the clock  */
    uint64_t jog_bend_ticks, jog_scrub_ticks;
    /* CUE button behaviour, see cdj_dsp_engine_key(). */
    bool cue_down, previewing;
    int64_t cue_until_ms;       /* a held key is re-sent; silence = released */
    uint64_t last_pos_ms;
    int64_t last_move_ms;       /* when the heard position last changed      */
    double heard_ms;            /* see cdj_dsp_engine_note_heard()           */
} CdjDspEng;

#define CDJ_STATEOUT_PNL_FRAME 0x28     /* = CDJ_PNL_FRAME, defined below */

#define C6X_L2_BASE     0x00800000u
#define C6X_L2_ALIAS    0x10800000u
#define C6X_L2_SIZE     0x00100000u
#define C6X_DDR_BASE    0x80000000u
#define C6X_DDR_SIZE    0x10000000u
#define C6X_PIN_ACK     21
#define C6X_PIN_READY   24
#define C6X_PIN_BOOTED  25
#define C6X_I2C_ADDR    0x30
#define C6X_PFC_PHDR    0x12E
#define C6X_PFC_PUDR    0x162
#define C6X_PFC_PVDR    0x164
#define C6X_PFC_PZDR    0x16C
#define C6X_RXQ         4096        /* words the DSP clocked that MAIN has not read */
#define C6X_TXFRAMES    8
#define C6X_TXWORDS     128
#define C6X_WINDOW      64          /* halfwords per DSP exchange */
/* A new exchange starts after a pause: words inside one are ~2 us apart. */
#define C6X_WINDOW_GAP_NS 20000
/* CDJ_C6X_RX_PACE starts after this many whole frames: the link is up. */
#define C6X_PACE_AFTER_FRAMES 64

typedef struct CdjC6x {
    bool on;
    c66x_core *core;
    c6655_soc *soc;
    uint8_t *l2, *ddr;
    QEMUTimer *tick;
    int64_t quantum_ns;
    uint64_t mhz;
    uint64_t mhz_run;               /* CDJ_C6X_MHZ, taken up once stage 1 boots */
    uint64_t max_catchup_ns;
    bool running;
    bool halted;

    /* CDJ_C6X_THREAD: the core and SoC run on their own thread under run_lock;
     * the link queues and the uPP handoff are shared under link_lock, and
     * anything touching guest state goes back through `kick` under the BQL.
     * Lock order: BQL -> run_lock -> link_lock; the DSP thread never takes the
     * BQL. */
    bool threaded;
    /* CDJ_C6X_THREAD=2: the DSP thread runs exactly the quantum the virtual-clock
     * tick hands it, [now, sync_target), in parallel with MAIN's quantum, and
     * the next tick waits until it has. No slip, one quantum of lead at most. */
    bool sync;
    uint64_t sync_target;
    QemuCond go_cond, done_cond;
    uint64_t sync_waits_ns;
    /* Wait accounting. The tick runs on the vCPU thread, so its CPU time is
     * MAIN's; dsp_idle_ns is the DSP thread waiting on MAIN. */
    uint64_t vcpu_cpu_ns, dsp_cpu_ns, dsp_idle_ns, ship_wait_ns;
    bool tick_tid_logged;
    /* CDJ_C6X_SHIP_ASYNC=1 (THREAD=2 only): queue uPP ships to the DSP
     * thread instead of taking run_lock; MAIN's frames are held until the DSP
     * has taken the bytes (ship_pending).
     * CDJ_C6X_SLACK_US=n (THREAD=2, implies SHIP_ASYNC): the tick only waits
     * until the DSP is within n us of MAIN. */
    bool ship_async;
    unsigned ship_pending;
    uint64_t slack_ns;
    uint64_t dsp_now_pub;
    QemuSemaphore go_sem, done_sem;
    bool quit;
    QemuThread thread;
    QemuMutex run_lock, link_lock;
    QEMUBH *kick;
    int ready_level, booted_level;  /* published pin levels for the PFC */
    GQueue *upp_in;                 /* GByteArray per CS6 transfer */
    uint64_t lag_max_ns, waits, chunks_run;
    int64_t epoch_ns;               /* virtual time at DSP time zero */
    uint64_t cyc_rem;               /* sub-nanosecond carry of the cycle clock */
    int64_t start_virt_ns;

    /* reset line and ROM I2C loader */
    int pth0;
    bool preload;
    GByteArray *i2c;                /* the current block, header included */
    GByteArray *table;              /* block payloads after the two parameter blocks */
    unsigned i2c_blocks, i2c_bad_ck;
    bool loaded;

    /* handshake */
    int ack;
    bool booted_latch;
    int64_t booted_virt_ns;
    unsigned windows_seen;

    /* uPP */
    uint64_t upp_bytes, upp_ships, upp_peak_queue;

    /* link: DSP -> MAIN */
    uint16_t rxq[C6X_RXQ];
    /* CDJ_C6X_RX_PACE=1 (THREAD=2 only): a word reaches MAIN no earlier than
     * the DSP time it was clocked at, so the id-1 frames carrying the play
     * position arrive spread out, not as one burst per quantum. The DSP runs
     * one extra quantum ahead to compensate. */
    uint64_t rx_stamp[C6X_RXQ];     /* DSP ns each rxq word was clocked at */
    bool rx_pace;
    uint64_t rx_pace_grain_ns;      /* CDJ_C6X_RX_PACE_GRAIN_US: release early */
    QEMUTimer *rx_timer;            /* releases a held frame, virtual clock */
    uint64_t rx_held, rx_late, rx_late_max_ns;
    unsigned rx_head, rx_len;
    uint64_t rx_words, rx_dropped, rx_windows;
    uint64_t rx_arm1, rx_armn, rx_goodframes, rx_aacc;
    uint16_t dump[64];
    uint64_t rx_kind[64];           /* reply id (byte2 & 0x3F) of frames MAIN read */
    uint64_t id1_p10_nonzero;
    uint32_t pchit_pc[16];
    uint64_t pchit_count[16];
    uint32_t pchit_b3[16];
    unsigned pchit_n;
    uint64_t watch_stores;
    int64_t watch_after_ms;
    uint32_t sample_addr[12], sample_size[12], sample_last[12];
    unsigned sample_n, sample_logged, sample_max;
    int64_t sample_next_ms, sample_every_ms, sample_after_ms;
    uint16_t id1_p23[2];
    unsigned id1_p23_logged;
    uint32_t watch_tag;             /* bit 31 set while a watched read is pending */
    unsigned watch_logged;
    /* tags of MAIN's 0x0AF0 reads: tag | arg index << 16 | valid bit */
    uint32_t lane_a_read_tag[16];
    unsigned lane_a_read_n;
    uint32_t arm_tag[16];           /* type-6 requests awaiting their reply */
    unsigned arm_n;
    uint64_t tx_hold_until_ns, ship_hold_ns;
    unsigned ship_logged;
    uint64_t magic[4];              /* AACC, CCAA, 5533, 3355 as the DSP sent them */
    CdjDma1State *dma;
    unsigned rx_ch;
    uint32_t rx_want;               /* words the armed ch5 transfer needs, 0 if none */
    uint64_t rx_pending_since;
    /* link: MAIN -> DSP */
    uint16_t txf[C6X_TXFRAMES][C6X_TXWORDS];
    unsigned txf_len[C6X_TXFRAMES];
    unsigned tx_head, tx_count;
    int tx_cur;                     /* frame being clocked out, -1 none */
    unsigned tx_pos;
    unsigned tx_ch;
    bool tx_waiting;
    uint64_t tx_frames, tx_frames_sent, tx_idle_words;
    uint64_t last_word_ns;
    unsigned wpos;                  /* word index inside the current exchange */
    uint64_t other_cs_words;
    uint32_t csnr_seen[256];

    /* audio */
    FILE *pcm;
    uint64_t pcm_words[2];

    /* accounting */
    uint64_t cycles, host_ns, slipped_ns, ticks, idle_skips, idle_skipped_ns;
    uint64_t intc_flag_writes;
    unsigned out_start_logs;
    struct {
        uint32_t mask;
        uint64_t n[2];              /* [0] CLR_DATA writes, [1] SET_DATA writes */
        uint32_t last_pc[2];
    } gpio_census[16];
    /* CDJ_C6X_PROF: where the DSP thread's host time goes, without perf. */
    bool prof;
    uint64_t prof_steps, prof_step_ns, prof_soc_ns, prof_drain_ns, prof_bh;
    uint64_t pcm_nonzero;
    GRand *jitter;
    uint64_t ahead_ns;              /* how far the DSP thread may lead MAIN */
    uint64_t prof_inval, prof_inval_ns, prof_spi, prof_spi_ns, prof_pcm_ns;
    int64_t report_at;
    c66x_stop last_stop;
    Notifier exit;
} CdjC6x;

#define CDJ_PFC_BASE    0xA4050000
#define CDJ_PFC_SIZE    0x1000
#define CDJ_LED_OFF     0x130
#define CDJ_LED_BIT     0x08

typedef struct CdjPfcState {
    MemoryRegion iomem;
    uint8_t reg[CDJ_PFC_SIZE];
    int last_led;
    int64_t last_ns;
    unsigned edges;
    unsigned wr_count[CDJ_PFC_SIZE];
    unsigned distinct[CDJ_PFC_SIZE];
} CdjPfcState;

#define CDJ_SCIFA_SIZE      0x100
#define SCIFA_SCASSR        0x14
#define SCIFA_SCAFDR        0x1C
#define SCIFA_SCAFTDR       0x20
#define SCIFA_SCAFRDR       0x24
#define SCIFA_TX_READY      0x0060      /* TEND | TDFE */

/* Accesses traced per SCIFA channel before going quiet. */
#define SCIFA_TRACE_MAX     64


typedef struct CdjScifaState {
    MemoryRegion iomem;
    CharBackend chr;
    const char *name;
    Notifier exit;
    unsigned traced;
    uint64_t reads, writes, txbytes;
    uint16_t reg[CDJ_SCIFA_SIZE / 2];
    FILE *cap;                  /* CDJ_CAPTURE sink, opened lazily */
} CdjScifaState;

/* ---------------------------------------------------------------------------
 * MSIOF, clocked serial interface with FIFO (manual section 25).
 *
 * SICTR enable bits must read back after a write (manual 25.3.7); the
 * firmware sets TXE and spins at 0x0838DF24 until it does. Mapped at the
 * physical address with raised priority, above the logged HPB window.
 */
#define CDJ_MSIOF_SIZE      0x100
#define CDJ_MSIOF_SICTR     0x28    /* control: TXE bit 9, RXE bit 8      */
#define CDJ_MSIOF_SIFCTR    0x30    /* FIFO control: TFUA [26:20], RFUA [6:0] */
#define CDJ_MSIOF_SITFDR    0x50    /* transmit FIFO                      */
#define CDJ_MSIOF_SIRFDR    0x60    /* receive FIFO: nothing drives it    */

/* SIFCTR TFUA[26:20]: free transmit FIFO stages, 0x40 when empty. The DSP
 * command builder at 0x08326FCA requires exactly 0x40 before arming the
 * transmit DMA. */
#define CDJ_MSIOF_TFUA_EMPTY 0x40

/* SICTR TXRST/RXRST (bits 1:0) self-clear in hardware; the firmware spins
 * until they read 0, so they complete immediately here. */
#define CDJ_MSIOF_SICTR_RST 0x3

typedef struct CdjDspPeer {
    bool present;
    bool swap;
    bool debug;
    bool echo;           /* CDJ_DSP_ECHO: reflect the command in the reply */
    bool reply;          /* CDJ_DSP_REPLY: answer with a dispatchable frame */
    int64_t reply_ms;    /* CDJ_DSP_REPLY_MS: use the real id only after this */
    uint64_t reply_n;    /* CDJ_DSP_REPLY_N: dispatch only this many (0 = all) */
    uint64_t reply_skip; /* CDJ_DSP_REPLY_SKIP: id 0 for this many frames first */
    uint64_t reply_every;/* CDJ_DSP_REPLY_EVERY: a valid id only every k frames */
    uint64_t reply_prime;/* CDJ_DSP_REPLY_PRIME: this many at full rate first */
    uint64_t reply_seen; /* eligible frames counted for reply_every            */
    bool     clocklog;   /* CDJ_DSP_CLOCKLOG: stamp every dispatched reply     */
    int      key_off;    /* CDJ_DSP_REPLY_AFTER_KEY: arm on this panel key,    */
    int      key_val;    /* <off>:<val>; -1 = disabled                         */
    uint64_t reply_used;
    unsigned reply_id;   /* handler id 1..6 put in byte[2] bits 5..0        */
    unsigned reply_words;/* payload words; byte[3] = 2 x this               */
    bool reply_level;    /* byte[2] bit 7, toggled so it falls 1 -> 0       */
    bool tag;            /* CDJ_DSP_TAG: answer with the live request tag   */
    uint32_t tag_addr;   /* CDJ_DSP_TAG_ADDR: where the firmware keeps it   */
    unsigned tag_off;    /* CDJ_DSP_TAG_OFF: byte offset in the reply frame */
    uint16_t tag_last;   /* the value most recently echoed, for the report  */
    uint64_t tag_echoes;
    bool reply_vary;     /* CDJ_DSP_REPLY_VARY: payload word[1] changes     */
    uint16_t vary;       /* the changing value itself                       */
    unsigned reply_cnt;  /* CDJ_DSP_REPLY_CNT: payload words[2]+[3] sum     */
    unsigned reply_fill; /* CDJ_DSP_REPLY_FILL: every payload word          */
    unsigned msg20;      /* CDJ_DSP_MSG20: the transport gate, msg+20        */
    bool param;          /* CDJ_DSP_PARAM: a parameter block that remembers */
    bool param_type;     /* CDJ_DSP_PARAM_TYPE: also stamp w2 = 4 (read answer) */
    uint64_t param_writes;   /* (index,value) pairs seen on op2 commands    */
    uint64_t param_reads;    /* op4 read commands seen                      */
    uint64_t param_answered; /* replies whose value came out of the store   */
    uint32_t param_last_idx;
    uint32_t param_last_val;
    uint32_t param_last_read;    /* index of the most recent op4 READ        */
    uint64_t param_tag_miss;     /* replies whose tag named no known read    */
    /* Histogram of reply frame sizes (MAIN's rx DMA word count). */
    uint32_t fill_hist[65];      /* frame word count -> how many replies       */
    uint32_t fill_hist_pending[65]; /* ... of which had a read outstanding      */
    uint32_t fill_over;          /* frames longer than the histogram           */
    bool framemap;               /* CDJ_DSP_FRAMEMAP: stamp each slot with its
                                  * own offset, to find what MAIN reads       */
    uint64_t framemaps;
    bool param_vec;              /* CDJ_DSP_PARAM_VEC: answer the list in order */
    bool param_seq;              /* CDJ_DSP_PARAM_SEQ: one value per reply      */
    bool veclen_exact;           /* CDJ_DSP_VECLEN_EXACT: W = 2 + values        */
    bool taglog;                 /* CDJ_DSP_TAGLOG: trace the pending tag      */
    bool param_pair;             /* CDJ_DSP_PARAM_PAIR: reply = index,value    */
    uint64_t param_vec_slots;    /* value slots filled                          */
    uint64_t param_vec_short;    /* replies too small for the whole list        */
    bool param_last;             /* CDJ_DSP_PARAM_LAST: fall back to it      */
    bool param_always;           /* CDJ_DSP_PARAM_ALWAYS: answer every frame */
    bool param_ro;               /* CDJ_DSP_PARAM_RO: census only, never write */
    bool param_log;              /* CDJ_DSP_PARAM_LOG: one line per command,
                                  * with the virtual timestamp               */
    bool param_echo;             /* CDJ_DSP_PARAM_ECHO: answer reads of an index
                                  * MAIN wrote with that value (harmful, opt-in) */
    bool param_pending;          /* a READ is outstanding and unanswered     */
    bool param_ramp_all;         /* CDJ_DSP_PARAM_RAMP_ALL: ramp every index
                                  * MAIN never writes                        */
    uint64_t replies;
    uint16_t cmd[64];    /* the last command frame's payload              */
    unsigned cmdwords;
    uint64_t syncs;      /* one-word sync requests answered            */
    uint64_t frames;     /* n-word response frames handed to the DMAC  */
    uint64_t txframes;   /* command frames taken from the transmit DMA */
    uint64_t txwords;
    uint64_t deferred;
    int64_t frame_ns;    /* minimum wire time between two response frames */
    int64_t ready_at;
    Notifier exit;
} CdjDspPeer;

enum {
    CDJ_IIC_IRQ_AL = 0,
    CDJ_IIC_IRQ_TACK,
    CDJ_IIC_IRQ_WAIT,
    CDJ_IIC_IRQ_DTE,
    CDJ_IIC_NR_IRQ
};

enum {
    CDJ_INTC_NONE = 0,
    CDJ_TMU0_TUNI0, CDJ_TMU0_TUNI1, CDJ_TMU0_TUNI2,
    CDJ_TMU1_TUNI0, CDJ_TMU1_TUNI1, CDJ_TMU1_TUNI2,
    CDJ_IRQ0, CDJ_IRQ1, CDJ_IRQ2, CDJ_IRQ3,
    CDJ_IRQ4, CDJ_IRQ5, CDJ_IRQ6, CDJ_IRQ7,
    CDJ_USB0,
    CDJ_DMAC0A_DEI0, CDJ_DMAC0A_DEI1, CDJ_DMAC0A_DEI2, CDJ_DMAC0A_DEI3,
    CDJ_DMAC0A,                  /* priority group: all four share IPRE[15:12] */
    CDJ_DMAC1A_DEI0, CDJ_DMAC1A_DEI1, CDJ_DMAC1A_DEI2, CDJ_DMAC1A_DEI3,
    CDJ_DMAC1A,                  /* priority group: all four share IPRB[7:4]   */
    CDJ_IIC0_AL, CDJ_IIC0_TACK, CDJ_IIC0_WAIT, CDJ_IIC0_DTE,
    CDJ_IIC0,                    /* priority group: all four share IPRH[3:0]   */
    CDJ_IIC1_AL, CDJ_IIC1_TACK, CDJ_IIC1_WAIT, CDJ_IIC1_DTE,
    CDJ_IIC1,                    /* priority group: all four share IPRH[7:4]   */
    CDJ_DMAC1B_DEI4, CDJ_DMAC1B_DEI5,
    CDJ_DMAC1B,                  /* priority group: both share IPRK[11:8]      */
    CDJ_MSIOFI0,
    CDJ_ETHI,                    /* EtherMAC, vector H'D60, IPRJ [11:8]       */
    CDJ_INTC_NR_SOURCES
};

bool cdj_pnl_enabled(void);
void cdj_dirty_init(void);
void cdj_spilink_init(void);
extern CdjDspEng cdj_dsp_eng;
void cdj_dsp_engine_init(void);
int cdj_dsp_engine_key(unsigned off, unsigned mask, int64_t dur_ms,
                              bool held, int64_t now);
int cdj_dsp_engine_key_up(unsigned off, unsigned mask, int64_t now);
int cdj_dsp_engine_key_poll(int64_t now);
double cdj_dsp_engine_rate(void);
uint64_t cdj_dsp_engine_pos_ms(void);
void cdj_dsp_engine_ship(unsigned ch, uint32_t sar, uint32_t n);
void cdj_audio_queue_ship(unsigned ch);
void cdj_beat_drive(unsigned ch, uint32_t sar);
int32_t cdj_pitch_pct(void);
void cdj_pitch_drive(unsigned ch, uint32_t sar);
void cdj_stateout_frame(unsigned ch, uint32_t sar);
void cdj_stateout_panel(const uint8_t *frame);
void cdj_link_frame(unsigned ch, uint32_t sar);
void cdj_link_census(unsigned ch, uint32_t sar);
void cdj_link_ramp(unsigned ch, uint32_t sar);
void cdj_audio_drain_init(void);
void cdj_ch4_dump(unsigned ch, uint32_t sar, uint32_t count,
                         unsigned unit, int64_t ms);
void cdj_ch4_patch(unsigned ch, uint32_t sar, uint32_t count,
                          unsigned unit);
void cdj_audio_live_arm(void);
void cdj_audio_out(unsigned ch, uint32_t sar, uint32_t bytes);
extern CdjC6x cdj_c6x;
extern CdjDma1State *cdj_dma1_singleton;
bool cdj_c6x_on(void);
void cdj_dspau_arm(void);
void cdj_c6x_mcbsp_tx(void *opaque, unsigned port, uint32_t word,
                             unsigned bits);
void cdj_c6x_i2c_byte(uint8_t v);
void cdj_c6x_pfc_write(const uint8_t *reg);
uint8_t cdj_c6x_pfc_read(hwaddr off, uint8_t b);
void cdj_c6x_upp_ship(uint32_t sar, uint32_t bytes);
bool cdj_c6x_link_rx(CdjDma1State *s, unsigned ch, uint32_t dar,
                            unsigned dm, uint32_t count, uint32_t *dar_out);
bool cdj_c6x_link_tx(CdjDma1State *s, unsigned ch, uint32_t sar,
                            unsigned sm, uint32_t count, uint32_t *sar_out);
void cdj_mprof_arm(void);
void cdj_c6x_init(void);
void cdj_dma1_run(CdjDma1State *s, unsigned ch);
void cdj_dma1_init(MemoryRegion *sysmem, qemu_irq *dei);
extern CdjPfcState *cdj_pfc_singleton;
void cdj_pfc_init(MemoryRegion *sysmem);
extern Notifier cdj_console_exit;
extern Notifier cdj_ivt_exit;
extern Notifier cdj_pcring_exit;
void cdj_pcring_dump(Notifier *n, void *opaque);
void cdj_ivt_dump(Notifier *n, void *unused);
void cdj_console_dump(Notifier *n, void *unused);
extern CdjScifaState *cdj_scifa4;
uint64_t cdj_peer_tx_bytes(void);
extern CdjScifaState *cdj_scifa_last;
void cdj_scifa_init(MemoryRegion *sysmem, const char *name,
                           hwaddr addr, Chardev *chr);
extern CdjDspPeer cdj_dsp;
bool cdj_dsp_present(void);
int64_t cdj_dsp_ready_at(void);
void cdj_dsp_sent(int64_t now);
void cdj_dsp_defer(void);
void cdj_dsp_init(void);
extern bool cdj_dsp_key_armed;
void cdj_dsp_fill(uint8_t *out, unsigned words);
void cdj_capture_write(FILE **fp, const char *suffix,
                              const void *buf, size_t len);
void cdj_dsp_take(const uint8_t *in, unsigned words);
void cdj_msiof(MemoryRegion *sysmem, const char *name, hwaddr addr,
                      bool dsp);
void cdj_ata_init(MemoryRegion *sysmem);
uint64_t cdj_dsp_i2c_tx_bytes(void);
void cdj_iic(MemoryRegion *sysmem, const char *name, hwaddr addr,
                    unsigned ch, qemu_irq *irq);
void cdj_pnl_init(MemoryRegion *sysmem, hwaddr addr);
void cdj_usb_init(MemoryRegion *sysmem, qemu_irq irq);
void cdj_ether_init(MemoryRegion *sysmem, qemu_irq irq);
void cdj_hpb_probe_init(MemoryRegion *sysmem);
extern struct intc_desc cdj_intc;
void cdj_intc_init(MemoryRegion *sysmem, SuperHCPU *cpu);
void cdj_irq5_probe_init(void);
void cdj_intc_exit_report(void);
void cdj_panel_init(void);
void cdj_probe(MemoryRegion *sysmem, const char *name,
                      hwaddr base, hwaddr size);
void cdj_ivtw_init(MemoryRegion *sysmem);
void cdj_vclock_init(void);
#endif
