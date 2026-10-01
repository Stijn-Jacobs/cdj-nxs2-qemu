# SPDX-License-Identifier: GPL-2.0-or-later
# BF531 display-processor core (see bfin.h): plain C, no QEMU dependency of
# its own. bfinrun.c and the Makefile here are the offline runner only and
# are not part of this build.
sh4_ss.add(when: 'CONFIG_CDJ2000', if_true: files(
  'bfin_core.c', 'bfin_exec.c', 'bfin_dsp.c', 'bf531.c',
))
