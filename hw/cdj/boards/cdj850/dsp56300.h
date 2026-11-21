/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CDJ850_DSP56300_H
#define CDJ850_DSP56300_H
#include "cdj_common.h"

/*
 * DSP56371 (DSP56300 family) emulator core for the CDJ-850 audio DSP (IC301).
 *
 * Executes the four SHI-bootstrapped program segments (boot mode 5: SPI
 * slave, 24-bit count + address + data words, re-entering bootstrap ROM at
 * P:$FF0000 between stages), drives DSPDREQ (PE4 -> MAIN IRQ7), exchanges
 * 24-bit SHI command/status words with MAIN's SSU0, receives SSI0/SSI1 audio
 * via ESAI RX0/RX1, and outputs 44.1 kHz stereo samples via ESAI TX0 to
 * cdj_host_audio_put().
 */
void dsp56300_init(void);
void dsp56300_shi_tx(uint8_t byte);
uint8_t dsp56300_shi_rx(void);
void dsp56300_shi_cs(bool active);
void dsp56300_set_pf(uint16_t pfdrh, uint16_t pfdrl);
void dsp56300_esai_rx_push(unsigned ch, uint32_t word24);
void dsp56300_esai_rx_push_pair(bool has0, uint32_t w0, bool has1, uint32_t w1);
unsigned dsp56300_esai_rx_count(unsigned ch);

#endif
