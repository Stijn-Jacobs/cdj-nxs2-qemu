# SPDX-License-Identifier: GPL-2.0-or-later
"""What the views share: Pillow images to pygame surfaces and back, and the
keyboard as X11 keysyms.

The art is drawn with Pillow (art.py) and shown with pygame, which draws at the
display's own pixel density. The deck's key map takes X11 keysyms over VNC
(rfb.py); pygame gives its own key codes, so keysym() maps them.
"""

import pygame


def surface(img, like=None):
    """A pygame Surface of a PIL image; RGBA keeps its alpha. With `like`,
    an opaque image is converted to that surface's format, which makes blitting
    it a copy rather than a conversion each time."""
    if img.mode == "RGBA":
        return pygame.image.frombytes(img.tobytes(), img.size, "RGBA")
    if img.mode != "RGB":
        img = img.convert("RGB")
    surf = pygame.image.frombytes(img.tobytes(), img.size, "RGB")
    return surf.convert(like) if like is not None else surf


def image(surf):
    """A PIL RGB image of a pygame Surface."""
    from PIL import Image
    return Image.frombytes("RGB", surf.get_size(), pygame.image.tobytes(surf, "RGB"))


_KEYSYMS = {
    pygame.K_BACKSPACE: 0xFF08, pygame.K_TAB: 0xFF09, pygame.K_RETURN: 0xFF0D,
    pygame.K_ESCAPE: 0xFF1B, pygame.K_DELETE: 0xFFFF, pygame.K_INSERT: 0xFF63,
    pygame.K_HOME: 0xFF50, pygame.K_LEFT: 0xFF51, pygame.K_UP: 0xFF52,
    pygame.K_RIGHT: 0xFF53, pygame.K_DOWN: 0xFF54, pygame.K_PAGEUP: 0xFF55,
    pygame.K_PAGEDOWN: 0xFF56, pygame.K_END: 0xFF57,
    pygame.K_LSHIFT: 0xFFE1, pygame.K_RSHIFT: 0xFFE2, pygame.K_LCTRL: 0xFFE3,
    pygame.K_RCTRL: 0xFFE4, pygame.K_CAPSLOCK: 0xFFE5, pygame.K_LALT: 0xFFE9,
    pygame.K_RALT: 0xFFEA, pygame.K_LMETA: 0xFFE7, pygame.K_RMETA: 0xFFE8,
    pygame.K_KP_ENTER: 0xFF8D, pygame.K_KP_MULTIPLY: 0xFFAA, pygame.K_KP_PLUS: 0xFFAB,
    pygame.K_KP_MINUS: 0xFFAD, pygame.K_KP_PERIOD: 0xFFAE, pygame.K_KP_DIVIDE: 0xFFAF,
    pygame.K_KP_EQUALS: 0xFFBD,
}
_KEYSYMS.update({getattr(pygame, f"K_KP{n}"): 0xFFB0 + n for n in range(10)})
_KEYSYMS.update({getattr(pygame, f"K_F{n}"): 0xFFBD + n for n in range(1, 13)})


def keysym(key):
    """The X11 keysym of a pygame key code, or None for a key the deck has no
    use for. Printable keys are their unshifted character (Latin-1 keysyms are
    the character codes), with Shift sent as a key of its own -- QEMU's VNC
    server turns that into the same scancodes a shifted keysym would."""
    if key in _KEYSYMS:
        return _KEYSYMS[key]
    if 0x20 <= key <= 0x7E:
        return key
    return None


def key_name(key):
    """pygame's name for a key code ('f2', 'return', ...)."""
    return pygame.key.name(key)
