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
        self.expected = tuple(tuple(line.split(None, 1))
                              for line in values.get("MODEL_EXPECTED", "").splitlines() if line.strip())


def load(model_id=None):
    """The named profile (CDJ_MODEL, else cdj2000nxs2). Raises ModelError with
    the script's message on an unknown id or an incomplete profile."""
    model_id = model_id or os.environ.get("CDJ_MODEL") or DEFAULT
    path = os.path.join(models_dir(), model_id + ".conf")
    if not os.path.isfile(path):
        raise ModelError("unknown model '%s'; known: %s" % (model_id, "".join(m + " " for m in list_models())))
    with open(path, encoding="utf-8") as f:
        values = _parse(f.read())
    for v in REQUIRED:
        if not values.get(v):
            raise ModelError("model profile %s does not set %s" % (path, v))
    return Model(model_id, values)


def extract_dir(lay, model):
    """Where this model's images live: the checkout's extract/ (or the
    packaged program's firmware data folder) for the default model, else
    <root>/<model's extract path>, mirroring the repository layout."""
    if model.extract == "extract":
        return lay.extract
    return os.path.join(lay.root, model.extract)
