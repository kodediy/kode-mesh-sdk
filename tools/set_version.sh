#!/usr/bin/env bash
#
# Moves the SDK to one version for a release: every component's own
# `version: "X"`, every kodediy/mesh* dependency's `"==X"` in the manifests,
# MESH_VERSION_MAJOR, _MINOR and _PATCH in mesh.h, and the tag the README pins
# under kodediy/mesh (`version: vX`). kode-sdk's pin is not touched: it moves
# on its own, when this SDK is built against a newer kode-sdk.
# tools/check_example_pins.sh checks that they agree; the release workflow
# checks that they equal the tag.
#
#   usage: tools/set_version.sh <X.Y.Z>
#   exit:  0 done, 2 not a version, 1 the pins disagree afterwards
#
set -euo pipefail
cd "$(dirname "$0")/.."

v="${1:-}"
if ! [[ "$v" =~ ^([0-9]+)\.([0-9]+)\.([0-9]+)$ ]]; then
    echo "usage: tools/set_version.sh <X.Y.Z>" >&2
    exit 2
fi
major="${BASH_REMATCH[1]}"
minor="${BASH_REMATCH[2]}"
patch="${BASH_REMATCH[3]}"

for f in components/*/idf_component.yml; do
    sed -i.bak -E \
        -e "s/^version: \"[0-9.]+\"/version: \"$v\"/" \
        -e "/kodediy\/mesh[a-z_]*:/ s/\"==[0-9.]+\"/\"==$v\"/" \
        "$f"
    rm -f "$f.bak"
done

sed -i.bak -E \
    -e "s/^(#define MESH_VERSION_MAJOR +)[0-9]+/\1$major/" \
    -e "s/^(#define MESH_VERSION_MINOR +)[0-9]+/\1$minor/" \
    -e "s/^(#define MESH_VERSION_PATCH +)[0-9]+/\1$patch/" \
    components/mesh/include/mesh.h
rm -f components/mesh/include/mesh.h.bak

# Only the `version:` that follows `kodediy/mesh:` in the README's yaml block.
awk -v v="$v" '
    /^  kodediy\/mesh:/ { inmesh = 1 }
    /^  kodediy\/kode:/ { inmesh = 0 }
    inmesh && /^    version: v[0-9.]+$/ { sub(/v[0-9.]+$/, "v" v); inmesh = 0 }
    { print }
' README.md > README.md.tmp
mv README.md.tmp README.md

tools/check_example_pins.sh
