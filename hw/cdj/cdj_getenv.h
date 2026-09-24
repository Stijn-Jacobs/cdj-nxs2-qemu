/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CDJ_GETENV_H
#define CDJ_GETENV_H
/* Route getenv() through the CDJ_ENVCACHE cache in cdj_env.c. */
#define getenv(name) cdj_getenv(name)

#endif
