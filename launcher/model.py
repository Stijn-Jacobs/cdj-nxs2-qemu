# SPDX-License-Identifier: GPL-2.0-or-later
"""Model profiles: emulator/models/<id>.conf, one per supported player. The
firmware and launch scripts read the profile named by CDJ_MODEL (or --model),
so they carry no model specifics of their own. See models/README.md for what
each variable means.
"""

import os
import re

from .layout import Layout

DEFAULT = "cdj2000nxs2"

# Where the display board's own update is kept beside the main images, for a
# model whose display runs inside the MAIN emulator.
DISPLAY_UPD_IMAGE = "display.upd"

# Checked in this order, so a missing one is reported the same way cdj_model.sh
# reports it: the first field a profile leaves out, not every one of them.
REQUIRED = ("MODEL_TITLE", "MODEL_MAIN_MACHINE", "MODEL_EXTRACT", "MODEL_FW_VERSION",
            "MODEL_UPD", "MODEL_UPD_SHA256", "MODEL_MAIN_SECTION", "MODEL_MAIN_LZSS",
            "MODEL_FW_STEPS")

# A field starts at the beginning of a line; its value runs to the next one
# (or the end of the file), which lets MODEL_EXPECTED hold several lines
# inside one quoted string, the way the profile is written for a person to
# read.
_FIELD = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*)=", re.MULTILINE)


class ModelError(Exception):
    pass


def models_dir():
    return os.path.join(Layout().emu, "models")


def list_models():
    d = models_dir()
    return sorted(f[:-len(".conf")] for f in os.listdir(d) if f.endswith(".conf"))


def _parse(text):
    # A checkout with core.autocrlf can give the profile CRLF endings; a value
    # ending in a carriage return would name no machine.
    text = text.replace("\r\n", "\n")
    text = "\n".join(line for line in text.split("\n") if not line.lstrip().startswith("#"))
    fields = list(_FIELD.finditer(text))
    values = {}
    for i, m in enumerate(fields):
        end = fields[i + 1].start() if i + 1 < len(fields) else len(text)
        v = text[m.end():end].strip()
        if len(v) >= 2 and v[0] == v[-1] == '"':
            v = v[1:-1]
        values[m.group(1)] = v
    return values


class Model:
    def __init__(self, model_id, values):
        self.id = model_id
        self.title = values["MODEL_TITLE"]
        self.main_machine = values["MODEL_MAIN_MACHINE"]
        self.gui_machine = values.get("MODEL_GUI_MACHINE", "")
        self.extract = values["MODEL_EXTRACT"]
        self.fw_version = values["MODEL_FW_VERSION"]
        self.upd = values["MODEL_UPD"].split()
        self.upd_sha256 = values["MODEL_UPD_SHA256"].split()
        self.main_section = values["MODEL_MAIN_SECTION"]
        self.gui_section = values.get("MODEL_GUI_SECTION") or "1"
        self.main_lzss = values["MODEL_MAIN_LZSS"]
        self.fw_steps = values["MODEL_FW_STEPS"].split()
        self.display_upd = values.get("MODEL_DISPLAY_UPD", "")
        self.launch = values.get("MODEL_LAUNCH") or "rig"
        self.dsp_idle = values.get("MODEL_DSP_IDLE", "")
        self.dsp_isr_fast = values.get("MODEL_DSP_ISR_FAST", "")
        self.dsp_gen_args = values.get("MODEL_DSP_GEN_ARGS", "")
        self.idle_s = values.get("MODEL_IDLE_S", "")
        self.load_steps = values.get("MODEL_LOAD_STEPS", "")
        self.expected = tuple(tuple(line.split(None, 1))
                              for line in values.get("MODEL_EXPECTED", "").splitlines() if line.strip())

    @property
    def is_rig(self):
        """The two-board real-DSP rig (rig.py); any other model starts as one
        MAIN emulator (deck.py)."""
        return self.launch == "rig"

    @property
    def has_dsp_module(self):
        """A one-window model whose DSP runs through a generated module."""
        return not self.is_rig and bool(self.dsp_idle)

    @property
    def module_dir(self):
        """The folder of the jit cache that holds this model's built module;
        the default model's is the one build_dsp_module.sh calls curated."""
        return "curated" if self.id == DEFAULT else "curated-" + self.id

    @property
    def images(self):
        """What a firmware install leaves in the extract folder."""
        # A profile that has not pinned its images yet still needs the kernel.
        names = tuple(os.path.basename(rel) for rel, _ in self.expected) or ("main_unpacked.bin",)
        return names + (DISPLAY_UPD_IMAGE,) if self.display_upd else names


def load(model_id=None):
    """The named profile (CDJ_MODEL, else cdj2000nxs2). Raises ModelError with
    the script's message on an unknown id or an incomplete profile."""
    model_id = (model_id or os.environ.get("CDJ_MODEL") or DEFAULT).strip().lower()
    path = os.path.join(models_dir(), model_id + ".conf")
    if not os.path.isfile(path):
        raise ModelError("unknown model '%s'; known: %s" % (model_id, "".join(m + " " for m in list_models())))
    with open(path, encoding="utf-8") as f:
        values = _parse(f.read())
    for v in REQUIRED:
        if not values.get(v):
            raise ModelError("model profile %s does not set %s" % (path, v))
    launch = values.get("MODEL_LAUNCH") or "rig"
    if launch not in ("rig", "deck"):
        raise ModelError("model profile %s: MODEL_LAUNCH is '%s', not rig or deck" % (path, launch))
    # The rig reads the images from extract/ itself, and a deck model sharing
    # that folder would overwrite the NXS2's firmware.
    if (launch == "rig") != (values["MODEL_EXTRACT"] == "extract"):
        raise ModelError("model profile %s: a rig model installs to extract, a deck model to its own folder "
                         "(MODEL_LAUNCH=deck is missing, or MODEL_EXTRACT is wrong)" % path)
    return Model(model_id, values)


def extract_dir(lay, model):
    """Where this model's images live: the checkout's extract/ (or the
    packaged program's firmware data folder) for the default model, else
    <root>/<model's extract path>, mirroring the repository layout."""
    if model.extract == "extract":
        return lay.extract
    return os.path.join(lay.root, model.extract)
