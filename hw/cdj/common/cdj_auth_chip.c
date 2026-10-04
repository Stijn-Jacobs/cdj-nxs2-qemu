/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj_common.h"
#include "cdj_auth_chip.h"
#include "cdj_getenv.h"

void cdj_auth_chip_init(CdjAuthChip *chip, const CdjAuthAnswer *answers,
                        unsigned n)
{
    const char *e = getenv("CDJ_IIC_RX_BYTE");
    unsigned i;

    memset(chip->reply, e ? (uint8_t)strtoul(e, NULL, 0) : 0xFF,
           sizeof(chip->reply));
    for (i = 0; i < n; i++) {
        chip->reply[answers[i].cmd] = answers[i].answer;
    }
    e = getenv("CDJ_IIC_REPLY");
    while (e && *e) {
        char *end;
        unsigned long cmd = strtoul(e, &end, 0);

        if (end == e || *end != ':') {
            break;      /* not a cmd:answer pair -- stop rather than spin */
        }
        e = end + 1;
        chip->reply[cmd & 0xFF] = (uint8_t)strtoul(e, &end, 0);
        if (end == e) {
            break;
        }
        e = end;
        while (*e == ',' || *e == ' ') {
            e++;
        }
    }
}
