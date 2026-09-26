# SPDX-License-Identifier: GPL-2.0-or-later
# Sourced, not run. Loads a model profile from models/<id>.conf into MODEL_*
# variables, so the firmware and launch scripts carry no model specifics.
#
#   cdj_model_load [id]   id defaults to $CDJ_MODEL, else cdj2000nxs2
#   cdj_model_list        the ids that have a profile

CDJ_MODELS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../models" && pwd)"

cdj_model_list() {
    local f
    for f in "$CDJ_MODELS_DIR"/*.conf; do
        f="${f##*/}"
        printf '%s\n' "${f%.conf}"
    done
}

cdj_model_load() {
    local id="${1:-${CDJ_MODEL:-cdj2000nxs2}}"
    local f="$CDJ_MODELS_DIR/$id.conf"

    if [ ! -f "$f" ]; then
        echo "unknown model '$id'; known: $(cdj_model_list | tr '\n' ' ')" >&2
        return 1
    fi
    # Clear the previous profile so an unset field cannot leak across models.
    unset MODEL_TITLE MODEL_MAIN_MACHINE MODEL_GUI_MACHINE MODEL_EXTRACT \
          MODEL_FW_VERSION MODEL_UPD MODEL_UPD_SHA256 MODEL_MAIN_SECTION \
          MODEL_GUI_SECTION MODEL_MAIN_LZSS MODEL_FW_STEPS MODEL_EXPECTED
    # A checkout with core.autocrlf can give the profile CRLF endings; a
    # value ending in a carriage return would name no machine.
    . <(tr -d '\r' < "$f")
    MODEL_ID="$id"
    local v
    for v in MODEL_TITLE MODEL_MAIN_MACHINE MODEL_EXTRACT MODEL_FW_VERSION \
             MODEL_UPD MODEL_UPD_SHA256 MODEL_MAIN_SECTION MODEL_MAIN_LZSS \
             MODEL_FW_STEPS; do
        if [ -z "${!v:-}" ]; then
            echo "model profile $f does not set $v" >&2
            return 1
        fi
    done
}
