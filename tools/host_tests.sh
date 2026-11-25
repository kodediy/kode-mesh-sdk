#!/usr/bin/env bash
#
# The host suites: every test that does not need the module.
#
# Nine projects, in this order. The plain-C rules (tests/host); the LR2021's
# driver over a model of the chip at the level of SPI frames (tests/host_drv);
# the owner as it is (mesh_aos.c) over a model of the radio and a stand-in for
# its Active Object (tests/host_owner); the owner's Active Object on
# kode-sdk's QP/C (tests/host_ao); the standard layer, mesh.h, over a faked
# owner (tests/host_standard); the extended layer, mesh_sys.h, on kode-sdk's
# QP/C over a faked owner (tests/host_sys); examples/console_actions's rows over a
# mesh_sys.h that writes down what it is asked (tests/host_console); and the
# plan built with CONFIG_MESH_LAB (tests/host_lab)
# and with the datasheet's power rows on as well (tests/host_lab_pa). C allows
# one definition per binary, so a seam that one suite fakes and another tests
# for real gets a project of its own.
#
#   usage: tools/host_tests.sh [--sanitize]
#          MESH_HOST_TEST_MAX_S=<seconds> tools/host_tests.sh    (600 by default)
#
# --sanitize builds every project in its own build_san directory with
# AddressSanitizer and UndefinedBehaviorSanitizer on the code under test (see
# tests/sanitize.cmake) and runs it; a memory error or undefined
# behaviour aborts the case. The plain build is left alone.
#
# A test binary gets MESH_HOST_TEST_MAX_S seconds to finish and is stopped past
# them: one that hangs would hold the run for ever, and the FreeRTOS simulator
# these suites run on can hang on a machine that is busy with something else.
#   exit:  0 every case passed, 1 something failed, hung or a suite shrank
#
set -uo pipefail
cd "$(dirname "$0")/.."

# "<project directory>:<elf name>", in the order they run.
PROJECTS=(
    "tests/host:mesh_host_tests"
    "tests/host_drv:mesh_drv_tests"
    "tests/host_owner:mesh_owner_tests"
    "tests/host_ao:mesh_ao_tests"
    "tests/host_standard:mesh_standard_tests"
    "tests/host_sys:mesh_sys_tests"
    "tests/host_console:mesh_console_tests"
    "tests/host_lab:mesh_lab_tests"
    "tests/host_lab_pa:mesh_lab_pa_tests"
)

if [ -z "${IDF_PATH:-}" ]; then
    printf '\033[31mFAIL\033[0m  ESP-IDF is not activated.\n'
    printf '      . <path-to-esp-idf>/export.sh\n'
    exit 1
fi

# idf.py refuses a target that contradicts IDF_TARGET in the environment, and
# esp-idf-ci-action exports one. The target is not a choice this script offers.
unset IDF_TARGET

max_s="${MESH_HOST_TEST_MAX_S:-600}"
total_ran=0
out_dir=build
if [ "${1:-}" = "--sanitize" ]; then
    out_dir=build_san
    export MESH_SANITIZE=1
    # Leaks off: the cases keep their fixtures for the life of the process.
    export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0:strict_string_checks=1"
    export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1"
fi

for entry in "${PROJECTS[@]}"; do
    proj="${entry%%:*}"
    elf="$proj/$out_dir/${entry##*:}.elf"

    # How many cases the sources declare, compared with how many ran: a case
    # file the link drops still leaves Unity printing OK over the rest.
    declared=$(grep -rh --include='*.c' -c 'TEST_CASE(' "$proj/cases" \
               | awk '{ n += $1 } END { print n + 0 }')

    # A build directory configured by another ESP-IDF refuses this one.
    cache="$proj/$out_dir/CMakeCache.txt"
    if [ -f "$cache" ] && ! grep -q "^PYTHON:.*=$(command -v python)$" "$cache"; then
        rm -rf "${proj:?}/$out_dir"
    fi

    if ! grep -q '^CONFIG_IDF_TARGET="linux"' "$proj/sdkconfig" 2>/dev/null; then
        if ! idf.py -C "$proj" -B "$proj/$out_dir" --preview set-target linux > "$proj/build.log" 2>&1; then
            printf '\033[31mFAIL\033[0m  %s: could not set the linux target\n\n' "$proj"
            tail -30 "$proj/build.log"
            exit 1
        fi
    fi

    # The case list is globbed and CONFIGURE_DEPENDS is not available to an
    # IDF component, so an added case would be left out of an incremental
    # build. Watch the list and reconfigure when it moves.
    stamp="$proj/$out_dir/.case-list"
    current=$(ls "$proj"/cases/test_*.c 2>/dev/null | sort)
    if [ ! -f "$stamp" ] || [ "$current" != "$(cat "$stamp" 2>/dev/null)" ]; then
        if ! idf.py -C "$proj" -B "$proj/$out_dir" reconfigure > "$proj/build.log" 2>&1; then
            printf '\033[31mFAIL\033[0m  %s: did not configure\n\n' "$proj"
            tail -30 "$proj/build.log"
            exit 1
        fi
        printf '%s' "$current" > "$stamp"
    fi

    if ! idf.py -C "$proj" -B "$proj/$out_dir" build > "$proj/build.log" 2>&1; then
        printf '\033[31mFAIL\033[0m  %s: did not build\n\n' "$proj"
        tail -30 "$proj/build.log"
        exit 1
    fi

    printf -- '-- %s --\n' "$proj"
    # In the background, with a clock on it: see MESH_HOST_TEST_MAX_S above.
    log="$proj/$out_dir/test.log"
    "$elf" < /dev/null > "$log" 2>&1 &
    pid=$!
    began=$(date +%s)
    hung=0
    while kill -0 "$pid" 2>/dev/null; do
        if [ $(( $(date +%s) - began )) -ge "$max_s" ]; then
            kill -9 "$pid" 2>/dev/null
            hung=1
            break
        fi
        sleep 1
    done
    wait "$pid" 2>/dev/null
    status=$?
    cat "$log"
    out=$(cat "$log")

    ran=$(echo "$out" | awk '/[0-9]+ Tests [0-9]+ Failures/ { print $1; exit }')

    if [ "$hung" -ne 0 ]; then
        printf '\n\033[31mFAIL\033[0m  %s: not finished after %s s, and stopped. The last case it began:\n' \
               "$proj" "$max_s"
        printf '      %s\n' "$(grep -a '^Running ' "$log" | tail -1)"
        exit 1
    fi
    if [ -z "$ran" ]; then
        printf '\n\033[31mFAIL\033[0m  %s: ended before its summary, with status %s. The last case it began:\n' \
               "$proj" "$status"
        printf '      %s\n' "$(grep -a '^Running ' "$log" | tail -1)"
        exit 1
    fi
    if [ "$status" -ne 0 ]; then
        printf '\n\033[31mFAIL\033[0m  %s: %s case(s) failed\n' "$proj" "$status"
        exit 1
    fi
    if [ "${ran:-0}" != "$declared" ]; then
        printf '\n\033[31mFAIL\033[0m  %s: %s cases declared, %s ran - the link dropped some\n' \
               "$proj" "$declared" "${ran:-0}"
        exit 1
    fi
    total_ran=$((total_ran + ran))
done

printf '\n\033[32mok\033[0m    %s cases across %s suites, all passing\n' \
       "$total_ran" "${#PROJECTS[@]}"
exit 0
