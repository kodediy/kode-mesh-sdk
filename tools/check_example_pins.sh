#!/usr/bin/env bash
#
# The kode-sdk version the projects pin, checked to be one.
#
# An example and the target build are each compiled against the
# kode-sdk commit (or tag) their own main/idf_component.yml names, not against
# the tree they sit in, and the README shows the same pin for someone to copy
# into an app of their own. When the pin moves in some of those and not in the
# others, the projects stop being the same test: the target builds on one
# kode-sdk and the examples on another, and what the README tells an app
# to use is neither. Bumping the pin is supposed to be a change to every one of
# them at once, and this makes that something CI asks for rather than
# something a commit remembers. (kode-sdk's own check of the same name also
# asks that the pin is not older than the newest release; that needs
# kode-sdk's tags, and this is another repository, so it asks less.)
#
# Three rules:
#   1. Every pin of kode-sdk says the same version. The pins are the
#      `version:` under `kodediy/kode` in every example, in tests/target and
#      in the README's yaml block, and KODE_SDK_VERSION in
#      tests/kode_sdk.cmake, which is the kode-sdk the host suites clone.
#   2. That version is a pin: a 40-digit commit, or a release tag `vX.Y.Z`.
#      A branch name is a pin to whatever it points at next.
#   3. This SDK's own version is one: `version: "X.Y.Z"` in every component's
#      manifest, `"==X.Y.Z"` in every pin of one component on another,
#      MESH_VERSION_MAJOR, _MINOR and _PATCH in mesh.h, and `version: vX.Y.Z`
#      under kodediy/mesh in the README's yaml block. A release moves them all.
#
# Today the pin is kode-sdk's release v0.1.5.
#
#   usage: tools/check_example_pins.sh
#   exit:  0 the pins agree and are pins and the SDK has one version, 1 otherwise
#
set -uo pipefail
cd "$(dirname "$0")/.."

# Every project that pins kode-sdk, found rather than listed, so a new one is
# checked the day it lands.
FILES=( examples/*/main/idf_component.yml
        tests/*/main/idf_component.yml
        README.md )

pins=""
for f in "${FILES[@]}"; do
    if [ ! -f "$f" ]; then
        printf '\033[31mFAIL\033[0m  %s: not there\n' "$f"
        exit 1
    fi
    # The pin is the first `version:` line inside the dependency's own block:
    # the lines indented deeper than the `kodediy/kode:` key, up to the next
    # key at its level. A file also holds `kodediy/mesh` and the others, whose
    # versions are not this one, and comments, which are skipped. The README
    # holds prose around its block, which never has the key on a line of its
    # own.
    v=$(awk '
        /^[[:space:]]*#/ { next }
        /^[[:space:]]*$/ { next }
        {
            match($0, /^[[:space:]]*/); ind = RLENGTH
            if ($0 ~ /^[[:space:]]+kodediy\/(kode|kdefs):[[:space:]]*$/) {
                inside = 1; key = ind; next
            }
            if (inside && ind <= key) { inside = 0 }
            if (inside && $0 ~ /^[[:space:]]+version:/) {
                line = $0
                sub(/^[[:space:]]+version:[[:space:]]*/, "", line)
                sub(/[[:space:]]*#.*$/, "", line)
                gsub(/["\047]/, "", line)
                sub(/[[:space:]]+$/, "", line)
                print line
                inside = 0
            }
        }' "$f")
    if [ -z "$v" ]; then
        printf '\033[31mFAIL\033[0m  %s: no `version:` under kodediy/kode (or kodediy/kdefs)\n' "$f"
        exit 1
    fi
    printf '      %-44s %s\n' "$f" "$v"
    pins="$pins$v"$'\n'
done

# The host suites do not go through the component manager: tests/kode_sdk.cmake
# clones kode-sdk at the tag it names. The same pin in another spelling, and
# the one a bump forgets, since no manifest holds it.
f=tests/kode_sdk.cmake
v=$(sed -n 's/^set(KODE_SDK_VERSION "\(.*\)")[[:space:]]*$/\1/p' "$f" 2>/dev/null)
if [ -z "$v" ]; then
    printf '\033[31mFAIL\033[0m  %s: no set(KODE_SDK_VERSION "...")\n' "$f"
    exit 1
fi
printf '      %-44s %s\n' "$f" "$v"
pins="$pins$v"$'\n'

distinct=$(printf '%s' "$pins" | sort -u)
if [ -z "$distinct" ]; then
    printf '\033[31mFAIL\033[0m  no pin of kode-sdk found anywhere\n'
    exit 1
fi
if [ "$(printf '%s\n' "$distinct" | wc -l)" -ne 1 ]; then
    printf '\033[31mFAIL\033[0m  the pins disagree; one kode-sdk, one version:\n'
    printf '%s\n' "$distinct" | sed 's/^/      /'
    exit 1
fi
pin="$distinct"

if ! printf '%s' "$pin" | grep -Eq '^([0-9a-f]{40}|v[0-9]+\.[0-9]+\.[0-9]+)$'; then
    printf '\033[31mFAIL\033[0m  %s is not a pin: name a 40-digit commit or a release tag vX.Y.Z\n' "$pin"
    exit 1
fi

printf '\033[32mok\033[0m    every project pins kode-sdk at %s\n' "$pin"

# ---- 3. This SDK's own version ------------------------------------------------
own=""
for f in components/*/idf_component.yml; do
    v=$(sed -n 's/^version: "\([0-9.]*\)"[[:space:]]*$/\1/p' "$f")
    if [ -z "$v" ]; then
        printf '\033[31mFAIL\033[0m  %s: no `version: "X.Y.Z"`\n' "$f"
        exit 1
    fi
    printf '      %-44s %s\n' "$f" "$v"
    own="$own$v"$'\n'
    # Every pin of a sibling in the same file.
    for s in $(sed -n 's/^[[:space:]]*kodediy\/mesh[a-z_]*:.*version: "==\([0-9.]*\)".*$/\1/p' "$f"); do
        own="$own$s"$'\n'
    done
    if grep -E '^[[:space:]]*kodediy/mesh[a-z_]*:' "$f" | grep -qv 'version: "=='; then
        printf '\033[31mFAIL\033[0m  %s: a sibling is not pinned with "==X.Y.Z"\n' "$f"
        exit 1
    fi
done

f=components/mesh/include/mesh.h
macro() { sed -n "s/^#define MESH_VERSION_$1 \([0-9]*\)U.*$/\1/p" "$f"; }
v="$(macro MAJOR).$(macro MINOR).$(macro PATCH)"
printf '      %-44s %s\n' "$f" "$v"
own="$own$v"$'\n'

# The README's pin of kodediy/mesh, in the block the kode-sdk pin was read from.
f=README.md
v=$(awk '
    /^[[:space:]]+kodediy\/mesh:[[:space:]]*$/ { inside = 1; next }
    inside && /^[[:space:]]+version:/ { sub(/^[[:space:]]+version:[[:space:]]*v/, ""); sub(/[[:space:]]+$/, ""); print; exit }' "$f")
printf '      %-44s %s\n' "$f" "${v:-none}"
own="$own${v:-none}"$'\n'

distinct=$(printf '%s' "$own" | sort -u)
if [ "$(printf '%s\n' "$distinct" | wc -l)" -ne 1 ]; then
    printf '\033[31mFAIL\033[0m  this SDK says more than one version of itself:\n'
    printf '%s\n' "$distinct" | sed 's/^/      /'
    exit 1
fi
if ! printf '%s' "$distinct" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$'; then
    printf '\033[31mFAIL\033[0m  %s is not a version X.Y.Z\n' "$distinct"
    exit 1
fi

printf '\033[32mok\033[0m    the SDK is at %s everywhere it says so\n' "$distinct"
exit 0
