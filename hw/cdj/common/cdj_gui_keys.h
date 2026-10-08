/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Host keyboard to front panel, shared by the boards that own a window. The
 * panel is on MAIN, so keys are forwarded over the socket in cdj_panelkeys.h.
 * Each window drives its own deck; the board supplies the key table because
 * the report bits differ between models.
 */
#ifndef CDJ_GUI_KEYS_H
#define CDJ_GUI_KEYS_H

#include "qemu/osdep.h"
#include "ui/input.h"

typedef struct {
    int qcode;
    unsigned off, mask;
    const char *name;
    bool confirmed;
} CdjGuiKey;

/* Register the keyboard handler when CDJ_PANEL_KEYSOCK is set. */
void cdj_gui_keys_init(const CdjGuiKey *keys, size_t count, const char *board);

/* Make the window's pointer absolute, so the host never grabs the mouse. */
void cdj_gui_pointer_init(void);

#endif /* CDJ_GUI_KEYS_H */
