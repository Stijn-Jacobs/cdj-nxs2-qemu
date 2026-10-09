# SPDX-License-Identifier: GPL-2.0-or-later
# M16C/63 display-processor core (see m16c.h): plain C, no QEMU dependency of
# its own. m16crun.c, the tests and the Makefile here are the offline runner
# only and are not part of this build.
sh4_ss.add(when: 'CONFIG_CDJ2000', if_true: files(
  'm16c_core.c', 'm16c_exec.c', 'm16c_decode.c', 'm16c63.c',
))
