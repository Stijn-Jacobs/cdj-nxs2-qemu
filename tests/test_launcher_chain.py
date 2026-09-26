# SPDX-License-Identifier: GPL-2.0-or-later
"""The scripts under scripts/run/ now run in launcher/; each QEMU must still be
started with the command line and environment the bash versions gave it.

REF_CHAIN_DIR names an emulator/ tree holding the bash versions (for one,
`git archive <commit> emulator/scripts`). The same run is made through both
trees against tests/fake_qemu.c, which records exactly what a native program
receives, and the two records are compared: every argument, and every
variable the boards read. Needs bash and a C compiler.
"""

import os
import shutil
import socket
import subprocess
import sys

import pytest

EMU = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if EMU not in sys.path:
    sys.path.insert(0, EMU)

from launcher import host  # noqa: E402

REF = os.environ.get("REF_CHAIN_DIR", "")
BASH = host.find_bash()
# Every variable the board sources and QEMU's CDJ patches read starts with one
# of these.
BOARD = ("CDJ_", "C66X_", "C5_", "C6655_", "SPILINK_", "USB_MEDIA", "CORE_TRACE", "REPLAY_", "SOC_TEST")
CC = next(([shutil.which(c)] for c in ("gcc", "cc", "clang") if shutil.which(c)), None)

pytestmark = pytest.mark.skipif(not (REF and BASH and CC), reason="needs REF_CHAIN_DIR, bash and a C compiler")

SCENARIOS = {
    "one-deck-sound-autojit": ("live.sh", 1, {"DJLINK": "0"}, False),
    "one-deck-djlink-module": ("live.sh", 1, {"NOSOUND": "1", "DJLINK": "1", "MEDIA_COPY": "1"}, True),
    "two-decks-knobs": ("live_linked.sh", 2, {"DJLINK": "1", "AUTOJIT": "1", "TBFAST": "0", "THIN": "5",
                                              "PERSIST": "1", "OWNMAC": "7", "ICOUNT": "shift=3",
                                              "AUDIODEV": "wav,path=/tmp/%TAG%.wav"}, True),
    "one-deck-app": ("live.sh", 1, {"DJLINK": "0", "GUI_DISPLAY": "vnc", "CDJ_APP_VNC_BASE": "5960"}, False),
}


@pytest.fixture(scope="module")
def fake_qemu(tmp_path_factory):
    exe = str(tmp_path_factory.mktemp("fakeqemu") / ("fake-qemu" + host.exe_suffix()))
    subprocess.run(CC + ["-O1", "-o", exe, os.path.join(EMU, "tests", "fake_qemu.c")], check=True)
    return exe


def _free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def _tree(tmp_path, module):
    root = tmp_path / "root"
    (root / "extract").mkdir(parents=True)
    for f in ("main_unpacked.bin", "gui_unpacked.bin", "flash.bin", "resblob.bin", "artblob.bin", "usbmedia3.img"):
        (root / "extract" / f).write_bytes(b"\0" * 64)
    home = tmp_path / "home"
    (home / "c14gen").mkdir(parents=True)
    if module:
        (home / "c14gen" / "curated").mkdir()
        (home / "c14gen" / "curated" / "m.so").write_bytes(b"\0")
    return str(root), str(home)


def _read(path):
    argv, env = [], {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            kind, _, rest = line.rstrip("\n").partition("\t")
            if kind == "A":
                argv.append(rest)
            elif kind == "E":
                k, _, v = rest.partition("=")
                env[k] = v
    return argv[1:], {k: v for k, v in env.items() if k.startswith(BOARD) and k != "CDJ_ROOT"}


def _run(tree, script, tag, ndecks, env):
    out = env["FAKE_QEMU_OUT"]
    os.makedirs(out)
    r = subprocess.run([BASH, host.posix(os.path.join(tree, "scripts", "run", script)), tag, str(ndecks)],
                       env=env, cwd=env["CDJ_ROOT"], capture_output=True, text=True, timeout=600)
    return {f: _read(os.path.join(out, f)) for f in os.listdir(out)}, r.stdout + r.stderr


@pytest.mark.parametrize("name", sorted(SCENARIOS))
def test_chain_matches_bash(name, tmp_path, fake_qemu):
    script, ndecks, extra, module = SCENARIOS[name]
    root, home = _tree(tmp_path, module)
    tag = "zpk" + "abcd"[sorted(SCENARIOS).index(name)]
    env = {k: v for k, v in os.environ.items() if not k.startswith(BOARD) and k not in ("MAIN_QEMU", "GUI_QEMU")}
    env.update(CDJ_ROOT=root, HOME=home, MAIN_QEMU=fake_qemu, GUI_QEMU=fake_qemu, PRIO="Normal",
               LAUNCH_STAGGER="0", FRAMES="1", MOTION_MS="1000", AUTOLOAD="0",
               GROUP="239.77.77.9:44990", RELAY_PORT=str(_free_port()), **extra)
    ref_env = dict(env, FAKE_QEMU_OUT=str(tmp_path / "ref"))
    if extra.get("GUI_DISPLAY") == "vnc":
        # The bash tree drew the app's screen through a GUI_QEMU wrapper.
        env.update(CDJ_APP_GUI_QEMU=fake_qemu, CDJ_APP_FRAME_DIR=host.native(str(tmp_path / "frames")))
        ref_env.update(env, GUI_QEMU=host.posix(os.path.join(REF, "app", "gui_vnc.sh")))
    want, ref_log = _run(REF, script, tag, ndecks, ref_env)
    got, log = _run(EMU, script, tag, ndecks, dict(env, FAKE_QEMU_OUT=str(tmp_path / "new")))
    expect = sorted("%s-%s%d.txt" % (r, tag, i) for i in range(1, ndecks + 1) for r in ("gui", "main"))
    assert sorted(want) == expect, ref_log[-3000:]
    assert sorted(got) == expect, log[-3000:]
    # The bash tree handed QEMU /tmp through MSYS2's conversion, which resolves
    # the directory's symlinks; the launcher names it as cygpath does.
    tmp = host.rig_tmp()
    norm = _spelling([tmp, os.path.realpath(tmp)])
    for f in expect:
        assert [norm(a) for a in got[f][0]] == [norm(a) for a in want[f][0]], f
        g = {k: norm(v) for k, v in got[f][1].items()}
        w = {k: norm(v) for k, v in want[f][1].items()}
        assert g == w, "%s: %s" % (f, [(k, w.get(k), g.get(k)) for k in sorted(set(g) | set(w)) if g.get(k) != w.get(k)])


HAS_MODELS = os.path.isfile(os.path.join(REF, "scripts", "cdj_model.sh")) if REF else False


@pytest.mark.skipif(not HAS_MODELS, reason="REF_COMMIT predates the model profiles")
def test_boot_deck_refuses_a_model_without_a_gui_board(tmp_path, fake_qemu):
    """A model still in bring-up (no GUI board, e.g. the CDJ-2000) makes
    boot_deck.sh refuse before it starts anything -- the same refusal, bash or
    Python."""
    root, home = _tree(tmp_path, module=False)
    env = {k: v for k, v in os.environ.items() if not k.startswith(BOARD) and k not in ("MAIN_QEMU", "GUI_QEMU")}
    env.update(CDJ_ROOT=root, HOME=home, MAIN_QEMU=fake_qemu, GUI_QEMU=fake_qemu, CDJ_MODEL="cdj2000")
    want, _ = _run(REF, "boot_deck.sh", "zpkm", 1, dict(env, FAKE_QEMU_OUT=str(tmp_path / "ref")))
    got, _ = _run(EMU, "boot_deck.sh", "zpkm", 1, dict(env, FAKE_QEMU_OUT=str(tmp_path / "new")))
    assert want == {}
    assert got == {}


def _spelling(aliases):
    aliases = sorted({a.replace("\\", "/").rstrip("/").lower() for a in aliases}, key=len, reverse=True)

    def norm(s):
        s = s.replace("\\", "/")
        low = s.lower()
        for a in aliases:
            i = low.find(a)
            while i >= 0:
                s = s[:i] + "<tmp>" + s[i + len(a):]
                low = s.lower()
                i = low.find(a, i + 5)
        return s
    return norm
