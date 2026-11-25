#!/usr/bin/env bash
#
# The layers, checked rather than described.
#
#   mesh        the standard layer: what a task calls, blocking          (mesh.h)
#   mesh_sys    the extended layer: what an Active Object calls, never
#               blocking - commands, requests, immediates, news      (mesh_sys.h)
#   mesh_aos    the owner: AO_Mesh, which holds the module and decides when;
#               both layers reach it through mesh_aos.h and nothing else
#   mesh_drv    the drivers: a part or a peripheral, nothing about who or when
#   mesh_bsp    the module described: positions, timings, the radio's limits
#
# Underneath, kode-sdk, reached through its public headers and nothing deeper.
# The rule not here - one owner for the module - is the owner's to keep: J3 is
# lent once, under MESH_BSP_LOAN_TAG, and a second loan answers KODE_EXISTS.
#
#   usage: tools/check_layers.sh
#   exit:  0 all clear, 1 something crossed a layer
#
set -uo pipefail
cd "$(dirname "$0")/.."

fail=0
report() {   # report <rule> <explanation> <matches>
    if [ -n "$3" ]; then
        printf '\n\033[31mFAIL\033[0m  %s\n      %s\n\n%s\n' "$1" "$2" "$3"
        fail=1
    else
        printf '\033[32mok\033[0m    %s\n' "$1"
    fi
}

# Build output and fetched components are skipped, and so are lines that look
# like prose: a comment that names esp_timer_get_time() is discussing it.
scan() { grep -rn --include='*.c' --include='*.h' --include='*.cpp' --include='*.hpp' \
             "$1" components \
             2>/dev/null \
           | grep -v '/build/' \
           | grep -v 'managed_components/' \
           | grep -vE ':[0-9]+: *(\*|//|/\*)' \
           | { [ $# -gt 1 ] && grep -vE "$2" || cat; }; }

# ---- 1. ESP-IDF stops at mesh_drv ------------------------------------------
# Above the drivers this is C that speaks kode_status_t. soc/ and hal/ are on
# the list because the register-level headers are not named esp_*.
report "1. ESP-IDF stops at mesh_drv" \
  "esp_*, driver/, soc/ or hal/ above the drivers. Put it behind a driver." \
  "$(scan '#include "\(esp_\|driver/\|soc/\|hal/\)' 'components/mesh_drv/')"

# ---- 2. FreeRTOS stops at mesh_aos -----------------------------------------
# The owner's callers wait for its answer on a semaphore, and its drivers
# share state with an interrupt under a spinlock; the facade asks the owner
# through mesh_aos's calls, never a queue of its own.
report "2. FreeRTOS stops at mesh_aos" \
  "freertos/ outside mesh_aos and mesh_drv. The two layers ask mesh_aos, which owns the module." \
  "$(scan '#include "freertos/' 'components/(mesh_aos|mesh_drv)/')"

# ---- 3. The module is described in one place ------------------------------
# A position, a timing or a radio limit is a fact about the Mesh module. Two
# copies of a hardware fact drift apart, and the module is driven by the
# wrong one - and a radio limit that drifts lets the module transmit past it.
report "3. The module is described in one place" \
  "Module facts and limits are defined only in mesh_bsp: mesh_bsp.h or mesh_region.h, and the name used everywhere else." \
  "$(scan '^#define MESH_\(BSP\|REGION\)_' 'components/mesh_bsp/')"

# ---- 4. The rules are plain C ----------------------------------------------
# mesh_plan (the owner's rules: power, duty, fingerprint), mesh_drv_lora
# (LoRa's arithmetic) and mesh_drv_nmea (the GNSS's sentences) are run as they
# are by tests/host. An ESP-IDF, FreeRTOS, kode.h or kode_sys.h include there
# would take them off the host.
report "4. The rules are plain C" \
  "mesh_plan, mesh_drv_lora and mesh_drv_nmea include neither ESP-IDF, FreeRTOS, kode.h nor kode_sys.h: they compute from what they are given." \
  "$(scan '#include "\(esp_\|driver/\|soc/\|hal/\|freertos/\|kode\.h\|kode_sys\.h\|kode_log\.h\)' \
     | grep -E 'components/mesh_(drv|aos)/[a-z0-9_]+/(include|src)/mesh_(plan|drv_lora|drv_nmea)\.[ch]:')"

# ---- 5. kode-sdk through its public headers --------------------------------
# kode.h, kode_sys.h, kode_defs.h and kode_log.h are kode-sdk's public surface.
# A layer header of kode-sdk (kaos, kqp, kdrv, khal, kbsp, kui) here couples
# this SDK to that SDK's insides, which change between releases. The one place
# QP/C is spoken is the owner's Active Object (mesh_ao.c), which includes kqp.h
# to make its queue and its pools; mesh_aos_events.c reads kqp_config.h for the
# level its events task runs at, if the release has one.
report "5. kode-sdk through its public headers" \
  "Only the kode_*.h headers. Use what kode.h or kode_sys.h expose; ask for it in kode-sdk if they do not." \
  "$(scan '#include "\(kaos\|kqp\|kdrv_\|khal_\|kbsp_\|kui_\)' 'components/mesh_aos/mesh/src/(mesh_ao|mesh_aos_events)\.c:')"

# ---- 6. Neither layer leaks the layers below -------------------------------
# What an app includes is mesh.h (a task's) or mesh_sys.h (an Active Object's),
# and neither names a driver or an owner type: the day a driver changes, no app
# has to.
report "6. The two layers do not leak the layers below" \
  "mesh.h and mesh_sys.h include nothing from mesh_aos or mesh_drv. Give the layer its own type for what the app needs to see." \
  "$(grep -Hn '#include "\(mesh_aos\|mesh_ao\.h\|mesh_plan\|mesh_drv_\)' \
         components/mesh/include/*.h components/mesh_sys/include/*.h 2>/dev/null)"

# ---- 7. QP/C stays in the Active Object -------------------------------------
# AO_Mesh is mesh_ao.c. What its signals do (mesh_aos.c), the drivers and the
# standard layer see no QP/C type: the seam is mesh_ao.h, and tests/host plays
# the AO behind it. The extended layer speaks QP/C types (QActive, QEvt) through
# kode_sys.h, which rule 13 fences.
report "7. QP/C stays in the Active Object" \
  "kqp.h, kode_ao.h and kode_worker.h only in mesh_ao.c. Put what needs QP/C there, behind a call in mesh_ao.h." \
  "$(scan '#include "\(kode_ao\|kode_worker\|kqp\)\.h"' | grep -v '^components/mesh_aos/mesh/src/mesh_ao\.c:')"

# ---- 8. One name, one function ---------------------------------------------
# The components are static libraries: a function of the same name in two of
# them links without a word, and the callers of one get the other. It
# happened in another module's SDK: the owner's scan reached the facade's
# function of the same name, which, called from the owner's own task, refused.
report "8. One name, one function" \
  "A function defined in two places. Give the lower layer's function its file's prefix (mesh_drv_lr2021_*, mesh_aos_*...)." \
  "$(find components -name '*.c' -not -path '*/build/*' -not -path '*/managed_components/*' -print0 \
       | xargs -0 grep -HoE '^[a-z_][a-z0-9_ *]*[ *]mesh_[a-z0-9_]+\(' 2>/dev/null \
       | grep -v ':static' | sed -E 's/^([^:]+):.*[ *](mesh_[a-z0-9_]+)\($/\2 \1/' \
       | sort | awk '{n[$1]++; f[$1]=f[$1]" "$2} END {for (k in n) if (n[k] > 1) print k ":" f[k]}')"

# ---- 9. One console name, one row ------------------------------------------
# kode-sdk's registry keeps a firmware's console functions and variables under
# one name each, [a-z0-9_] of 1..15 and never a function's and a variable's at
# once, each described in 48 characters at most. A table with one row against
# that is refused whole, and all that says so is a line on the log. It
# happened in another module's SDK: console_actions had a function and a
# variable of one name, and not one of its variables reached the console.
console_rows() {
    for example in examples/*/; do
        find "$example" -name '*.c' -not -path '*/build/*' -not -path '*/managed_components/*' -print0 \
          | xargs -0 awk -v example="${example%/}" '
              /kode_(function|variable)_t[ \t]+[A-Za-z0-9_]+\[\][ \t]*=/ { rows = 1 }
              rows && /^\};/ { rows = 0 }
              rows {
                  line = $0
                  while (match(line, /\.name[ \t]*=[ \t]*"[^"]*"/)) {
                      name = substr(line, RSTART, RLENGTH)
                      sub(/^[^"]*"/, "", name); sub(/"$/, "", name)
                      if (name !~ /^[a-z0-9_]+$/ || length(name) > 15)
                          print FILENAME ":" FNR ": \"" name "\" is not [a-z0-9_] of 1..15"
                      if (name in first)
                          print FILENAME ":" FNR ": \"" name "\" again, first at " first[name]
                      else
                          first[name] = FILENAME ":" FNR
                      line = substr(line, RSTART + RLENGTH)
                  }
                  line = $0
                  while (match(line, /\.desc[ \t]*=[ \t]*"[^"]*"/)) {
                      desc = substr(line, RSTART, RLENGTH)
                      sub(/^[^"]*"/, "", desc); sub(/"$/, "", desc)
                      if (length(desc) > 48)
                          print FILENAME ":" FNR ": a description of " length(desc) " characters, 48 at most"
                      line = substr(line, RSTART + RLENGTH)
                  }
              }'
    done
}
report "9. One console name, one row" \
  "Rename or shorten the row: one bad row and kodeOS refuses its whole table." \
  "$(console_rows)"

# ---- 10. The target build links every call of both layers -------------------
# tests/target names each mesh.h and mesh_sys.h function in its ALL[] so that
# every one, and everything below it, is compiled and linked for the esp32p4
# before it is first in somebody's app. A call added to a header and not there
# is not checked.
declared=$(cat components/mesh/include/mesh.h components/mesh_sys/include/mesh_sys.h \
           | grep -oE '^[A-Za-z_][A-Za-z0-9_ ]*[ *]mesh_[a-z0-9_]+\(' \
           | sed -E 's/.*[ *](mesh_[a-z0-9_]+)\($/\1/' | sort -u)
listed=$(grep -oE '\(const void \*\)mesh_[a-z0-9_]+' tests/target/main/main.c \
         | sed -E 's/.*\)//' | sort -u)
report "10. The target build links every call of both layers (ALL[])" \
  "Add it to ALL[] in tests/target/main/main.c." \
  "$(comm -23 <(printf '%s\n' "$declared") <(printf '%s\n' "$listed"))"

# ---- 11. mesh.h speaks no kode_status_t --------------------------------------
# The standard layer returns bool and says why in mesh_error(): a task's code
# reads `if (mesh_radio_send(...))`, not a status table. kode_status_t is the
# extended layer's (and the owner's) vocabulary.
report "11. mesh.h speaks no kode_status_t" \
  "mesh.h returns bool and mesh_error() says why. Keep kode_status_t in mesh_sys.h and the owner." \
  "$(grep -Hn 'kode_status_t\|KODE_OK\|KODE_DENIED' components/mesh/include/mesh.h \
       | grep -vE ':[0-9]+: *(\*|//|/\*)')"

# ---- 12. Every request takes the asking AO first ----------------------------
# A request in mesh_sys.h is a call whose answer is a signal, MESH_<VERB>_RSP_SIG,
# posted to the AO that asked: so it is the call's first argument, `QActive *me`,
# and the answer cannot go to an AO that was not named. The check goes both ways: a
# signal with no function of that name, a function without the asker, and an asker
# on a call no signal answers (but the subscription).
H=components/mesh_sys/include/mesh_sys.h
requests() {
    for sig in $(grep -oE 'MESH_[A-Z0-9_]+_RSP_SIG' "$H" | sort -u); do
        fn="mesh_sys_$(printf '%s' "$sig" | sed -E 's/^MESH_//; s/_RSP_SIG$//' | tr 'A-Z' 'a-z')"
        grep -qE "^kode_status_t ${fn}\(QActive \*me[,)]" "$H" \
            || echo "$H: ${sig} is answered, but ${fn}() does not take QActive *me first"
    done
    grep -oE '^kode_status_t mesh_sys_[a-z0-9_]+\(QActive \*me' "$H" \
      | sed -E 's/^kode_status_t (mesh_sys_[a-z0-9_]+)\(.*/\1/' \
      | grep -vE '^mesh_sys_(un)?subscribe$' \
      | while read -r fn; do
            sig="MESH_$(printf '%s' "${fn#mesh_sys_}" | tr 'a-z' 'A-Z')_RSP_SIG"
            grep -q "$sig" "$H" || echo "$H: ${fn}() takes an AO, but no ${sig} answers it"
        done
}
report "12. Every request in mesh_sys.h takes QActive *me first" \
  "A request is answered by a signal to the AO that asked: name it first, and give the answer a MESH_<VERB>_RSP_SIG." \
  "$(requests)"

# ---- 13. Only the extended layer and the owner speak kode_sys.h --------------
# kode_sys.h is kodeOS's extended layer: QP/C, the AOs' requests, every service. The
# standard layer, the drivers and the module's description stay on kode.h (or
# on nothing), so a task's app does not pull the extended layer in.
report "13. Only mesh_sys and mesh_aos include kode_sys.h" \
  "kode_sys.h outside components/mesh_sys and components/mesh_aos. The standard layer includes kode.h." \
  "$(scan '#include "kode_sys\.h"' 'components/(mesh_sys|mesh_aos)/')"

exit "$fail"
