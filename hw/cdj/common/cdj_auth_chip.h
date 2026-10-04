/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The authentication chip on the players' I2C bus (slave 0x10). A read
 * returns the answer for the command byte written in the transaction before
 * it, so the model is one byte of state and a table.
 */
#ifndef CDJ_AUTH_CHIP_H
#define CDJ_AUTH_CHIP_H
#include <stdint.h>

typedef struct CdjAuthChip {
    uint8_t cmd;                /* last data byte written to the chip      */
    uint8_t reply[256];         /* answer to a read, indexed by cmd        */
} CdjAuthChip;

typedef struct CdjAuthAnswer {
    uint8_t cmd;
    uint8_t answer;
} CdjAuthAnswer;

/* Fills the reply table: CDJ_IIC_RX_BYTE (default 0xFF, the data register's
 * reset value) for every command, then the board's answers, then the
 * CDJ_IIC_REPLY overrides (cmd:answer pairs, comma separated). */
void cdj_auth_chip_init(CdjAuthChip *chip, const CdjAuthAnswer *answers,
                        unsigned n);

static inline void cdj_auth_chip_write(CdjAuthChip *chip, uint8_t v)
{
    chip->cmd = v;
}

static inline uint8_t cdj_auth_chip_read(const CdjAuthChip *chip)
{
    return chip->reply[chip->cmd];
}

#endif
