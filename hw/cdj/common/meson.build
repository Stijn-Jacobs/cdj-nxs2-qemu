# SPDX-License-Identifier: GPL-2.0-or-later
# Shared by every CDJ MAIN board; each board's Kconfig entry selects CDJ_COMMON.
sh4_ss.add(when: 'CONFIG_CDJ_COMMON', if_true: files(
  'cdj_env.c', 'sh4_board.c', 'sh4_intc.c', 'sh4_periph.c', 'sh4a_dmac.c',
))
