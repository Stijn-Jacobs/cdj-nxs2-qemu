# SPDX-License-Identifier: GPL-2.0-or-later
# The CDJ-2000 and CDJ-2000NXS MAIN board (SH7763), in bring-up.
sh4_ss.add(when: 'CONFIG_CDJ2000', if_true: files(
  'board.c', 'sh7763_intc.c', 'board_gpio.c', 'panel.c',
  'dsp_host.c', 'dsp_c6747.c', 'dsp_edma3.c', 'dsp_c6727.c', 'dsp_dmax.c', 'dsp_audio_out.c',
  'display_bf531.c', 'display_link.c', 'sdhi.c', 'iic.c',
))
