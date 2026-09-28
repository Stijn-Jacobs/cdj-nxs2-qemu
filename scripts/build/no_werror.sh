# SPDX-License-Identifier: GPL-2.0-or-later
# Sourced by build_main.sh and build_display.sh, from inside the build tree,
# after configure and before ninja.
#
# Keeps -Werror off. configure writes "werror = true" into config-meson.cross
# for a git checkout on Linux and Windows; --disable-werror beats it at
# configure, but a meson regenerate (a cached or older tree whose sources
# changed) re-reads the file and stores werror=true again, and then one gcc
# warning in the board sources fails the build. Drop the line, and turn the
# option back off where a regenerate already turned it on.
if grep -q '^werror = true$' config-meson.cross 2>/dev/null; then
    sed -i.bak '/^werror = true$/d' config-meson.cross && rm -f config-meson.cross.bak
fi
if grep -q '"name": "werror", "value": true' meson-info/intro-buildoptions.json 2>/dev/null; then
    for _meson in pyvenv/bin/meson pyvenv/Scripts/meson.exe pyvenv/Scripts/meson; do
        [ -x "$_meson" ] && break
    done
    echo "werror was on in this build tree -- turning it off"
    "$_meson" configure -Dwerror=false .
fi
