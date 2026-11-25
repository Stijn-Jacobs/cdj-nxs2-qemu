# SPDX-License-Identifier: GPL-2.0-or-later
"""The keymaps QEMU's VNC server needs, and the dangling links a development
build leaves in their place.

The failure this guards against does not look like one: the display-board QEMU
exits with a keymap complaint, the deck waits for a screen that never appears,
and the window stays black. These tests keep the repair honest without needing a
QEMU build.
"""

import os
import sys

import pytest

EMU = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if EMU not in sys.path:
    sys.path.insert(0, EMU)

from launcher import qemudata  # noqa: E402


def _bundle_keymaps(eb_dir):
    return os.path.join(eb_dir, "qemu-bundle", "usr", "local", "share", "qemu", "keymaps")


def _source_keymaps(root):
    return os.path.join(root, "pc-bios", "keymaps")


def test_copies_real_files_over_the_dangling_links_a_dev_build_leaves(tmp_path):
    eb_dir = tmp_path / "qemu-build-eb"
    bundle = _bundle_keymaps(str(eb_dir))
    os.makedirs(bundle)
    # What the build leaves: links into a source tree whose keymaps were never
    # installed, so they resolve to nothing.
    os.symlink(str(tmp_path / "missing" / "en-us"), os.path.join(bundle, "en-us"))
    os.symlink(str(tmp_path / "missing" / "de"), os.path.join(bundle, "de"))

    source = tmp_path / "qemu-src"
    os.makedirs(_source_keymaps(str(source)))
    for name in ("en-us", "de"):
        with open(os.path.join(_source_keymaps(str(source)), name), "w") as f:
            f.write("keymap %s\n" % name)

    data = tmp_path / "data"
    written = qemudata.ensure_keymaps("unused-exe", str(eb_dir), source_tree=str(source),
                                      datadir=str(data))

    assert written == ["de", "en-us"]
    for name in ("en-us", "de"):
        target = os.path.join(str(data), "keymaps", name)
        assert os.path.isfile(target) and not os.path.islink(target)
        with open(target) as f:
            assert f.read() == "keymap %s\n" % name


def test_the_bundles_own_keymaps_win_over_the_source_trees(tmp_path):
    eb_dir = tmp_path / "qemu-build-eb"
    bundle = _bundle_keymaps(str(eb_dir))
    os.makedirs(bundle)
    with open(os.path.join(bundle, "en-us"), "w") as f:
        f.write("generated\n")

    source = tmp_path / "qemu-src"
    os.makedirs(_source_keymaps(str(source)))
    with open(os.path.join(_source_keymaps(str(source)), "en-us"), "w") as f:
        f.write("ready-made\n")

    data = tmp_path / "data"
    qemudata.ensure_keymaps("unused-exe", str(eb_dir), source_tree=str(source),
                            datadir=str(data))

    with open(os.path.join(str(data), "keymaps", "en-us")) as f:
        assert f.read() == "generated\n"


def test_is_happy_when_the_keymaps_are_already_in_place(tmp_path):
    """setup.sh is re-run often: the second run must not fail on its own work."""
    eb_dir = tmp_path / "qemu-build-eb"
    bundle = _bundle_keymaps(str(eb_dir))
    os.makedirs(bundle)
    with open(os.path.join(bundle, "en-us"), "w") as f:
        f.write("generated\n")

    first = qemudata.ensure_keymaps("unused-exe", str(eb_dir), datadir=str(bundle))
    second = qemudata.ensure_keymaps("unused-exe", str(eb_dir), datadir=str(bundle))

    assert first == second == ["en-us"]


def test_refuses_to_go_on_without_the_keymap_vnc_needs(tmp_path):
    eb_dir = tmp_path / "qemu-build-eb"
    bundle = _bundle_keymaps(str(eb_dir))
    os.makedirs(bundle)
    os.symlink(str(tmp_path / "missing" / "en-us"), os.path.join(bundle, "en-us"))

    with pytest.raises(RuntimeError) as err:
        qemudata.ensure_keymaps("unused-exe", str(eb_dir), datadir=str(tmp_path / "data"))
    assert "en-us" in str(err.value)


def test_data_dir_asks_the_binary_where_it_looks(tmp_path):
    exe = tmp_path / "qemu-system-sh4eb"
    datadir = tmp_path / "share" / "qemu"
    datadir.mkdir(parents=True)
    exe.write_text("#!/bin/sh\nprintf 'pc-bios\\n%s\\n' '%s'\n" % (datadir, datadir))
    exe.chmod(0o755)

    assert qemudata.data_dir(str(exe)) == str(datadir)
