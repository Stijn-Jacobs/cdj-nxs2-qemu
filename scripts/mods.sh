#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Sourced, not run. Reads mods.conf, the mods registry shared by setup.sh,
# start.sh and rig.sh (see that file for the row format).
MODS_CONF="${MODS_CONF:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/mods.conf}"

# mods_each <function>: calls <function> key env on off default description
# for every mod in the registry, in file order.
mods_each() {
    local key env on off def desc
    while IFS='|' read -r key env on off def desc; do
        [ -z "$key" ] && continue
        case "$key" in \#*) continue ;; esac
        "$1" "$key" "$env" "$on" "$off" "$def" "$desc"
    done < <(grep -v '^[[:space:]]*#' "$MODS_CONF" | grep -v '^[[:space:]]*$')
}

# mod_conf_key <env> -> the cdj.conf key this knob is saved under. A knob that
# is not already CDJ_-prefixed gets one.
mod_conf_key() {
    case "$1" in CDJ_*) printf '%s' "$1" ;; *) printf 'CDJ_%s' "$1" ;; esac
}

# mod_default <env> -> the value to fall back to when nothing else set that
# knob: this mod's own "on" or "off" value, whichever the registry defaults to.
mod_default() {
    local want="$1" found=""
    _mod_default_one() {
        [ "$2" = "$want" ] || return 0
        [ "$5" = on ] && found="$3" || found="$4"
    }
    mods_each _mod_default_one
    printf '%s' "$found"
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
# mods_snapshot found the caller's own environment had already set it. Call
# after sourcing cdj.conf.
mods_apply() {
    _mods_apply_one() {
        local key="$1" env="$2" on="$3" off="$4" def="$5" confkey saved marker
        marker="_MOD_CALLER_${env}"
        if [ -n "${!marker+x}" ]; then
            export "$env=${!marker}"
            unset "$marker"
            return 0
        fi
        [ -n "${!env+x}" ] && [ -n "${!env}" ] && return 0
        confkey="$(mod_conf_key "$env")"
        saved="${!confkey-}"
        case "$saved" in
            "$on" | "$off") export "$env=$saved" ;;
            *) [ "$def" = on ] && export "$env=$on" || export "$env=$off" ;;
        esac
    }
    mods_each _mods_apply_one
}
