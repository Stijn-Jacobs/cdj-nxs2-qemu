# SPDX-License-Identifier: GPL-2.0-or-later
# The CDJ-2000NXS2: MAIN (SH7724) and the GUI processor (SH7269).
sh4_ss.add(when: 'CONFIG_CDJ2000NXS2', if_true: files(
  'board.c',
  'dma1_link.c', 'dsp_audio.c', 'dsp_c6x.c',
  'ether.c', 'host_display.c', 'host_io.c', 'panel.c',
  'soc_ata.c', 'soc_iic.c', 'soc_intc.c', 'soc_msiof.c',
  'soc_pfc.c', 'soc_scifa.c',
  'usb_r8a66597.c',
  'diag/diag_dump.c', 'diag/diag_link.c', 'diag/diag_mainprof_win32.c', 'diag/diag_mem.c',
  'standin/standin_dsppeer.c', 'standin/standin_engine.c',
  'standin/standin_linkramp.c', 'standin/standin_mp3.c',
))
sh4_ss.add(when: 'CONFIG_SH7269GUI', if_true: files('sh7269gui.c'))
