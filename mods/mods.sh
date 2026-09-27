#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Sourced, not run. Reads mods.conf, the mods registry shared by setup.sh,
# start.sh, rig.sh and boot_deck.sh (see that file for the row format).
MODS_CONF="${MODS_CONF:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/mods.conf}"

# mods_each <function>: calls <function> key env on off default description
# for every mod in the registry, in file order.
mods_each() {
    local key env on off def desc
    # fd 3, so a <function> that prompts still reads the terminal on stdin.
    while IFS='|' read -r -u 3 key env on off def desc; do
        [ -z "$key" ] && continue
        case "$key" in \#*) continue ;; esac
        "$1" "$key" "$env" "$on" "$off" "$def" "$desc"
    done 3< <(grep -v '^[[:space:]]*#' "$MODS_CONF" | grep -v '^[[:space:]]*$')
}

# mod_conf_key <env> -> the cdj.conf key this knob is saved under. A knob that
# is not already CDJ_-prefixed gets one.
mod_conf_key() {
    case "$1" in CDJ_*) printf '%s' "$1" ;; *) printf 'CDJ_%s' "$1" ;; esac
}

# mods_snapshot: remember which knobs the caller's own environment already set
# (a bare env var, e.g. CDJ_GUI_FRAME_MS=3 ./start.sh), before sourcing
# cdj.conf, whose CDJ_-prefixed keys share those same names and would
# otherwise overwrite them just by being sourced. Call before ". cdj.conf".
mods_snapshot() {
    _mods_snapshot_one() {
        local env="$2"
        [ -n "${!env+x}" ] && export "_MOD_CALLER_${env}=${!env}"
    }
    mods_each _mods_snapshot_one
}

# mods_apply: export every mod's knob from cdj.conf's saved choice, unless
# mods_snapshot found the caller's own environment had already set it; a knob
# set nowhere gets the registry default. Call after sourcing cdj.conf.
mods_apply() {
    _mods_apply_one() {
        local key="$1" env="$2" on="$3" off="$4" def="$5" confkey saved marker
        marker="_MOD_CALLER_${env}"
        if [ -n "${!marker+x}" ]; then
            export "$env=${!marker}"
            unset "$marker"
            return 0
        fi
        # Sourcing cdj.conf sets a CDJ_ knob without exporting it.
        [ -n "${!env-}" ] && { export "${env?}"; return 0; }
        confkey="$(mod_conf_key "$env")"
        saved="${!confkey-}"
        case "$saved" in
            "$on" | "$off") export "$env=$saved" ;;
            *) [ "$def" = on ] && export "$env=$on" || export "$env=$off" ;;
        esac
    }
    mods_each _mods_apply_one
}
