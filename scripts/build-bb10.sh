#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
python3 "$ROOT/scripts/dependencies.py"

IMAGE=bb10-native-dev:local
docker image inspect "$IMAGE" >/dev/null

mkdir -p "$ROOT/build"
docker run --rm --network none --user "$(id -u):$(id -g)" \
    -v "$ROOT:/src" -w /src "$IMAGE" \
    sh /src/scripts/build-target-inside.sh

python3 "$ROOT/scripts/check-symbol-isolation.py" \
    "$ROOT/build/bb10/bb10_tls_probe" \
    "$ROOT/build/bb10/test_bb10_tls" \
    "$ROOT/build/bb10/test_standalone_consumer" \
    "$ROOT/build/bb10/test_network" \
    "$ROOT/build/bb10/test_target_coexistence" \
    "$ROOT/build/bb10/test_ui" \
    "$ROOT/build/bb10/todoist-passport"

python3 "$ROOT/scripts/check-package.py" "$ROOT/build/bb10/todoist-passport.bar"

BUILD_ID=$(python3 -c 'import xml.etree.ElementTree as ET; tree = ET.parse("bar-descriptor.xml"); root = tree.getroot(); el = root.find("{http://www.qnx.com/schemas/application/1.0}buildId"); print(el.text if el is not None else "")')
if [ -n "$BUILD_ID" ]; then
    cp "$ROOT/build/bb10/todoist-passport.bar" "$ROOT/build/bb10/todoist-passport-build-${BUILD_ID}.bar"
    echo "Created build artifact: build/bb10/todoist-passport-build-${BUILD_ID}.bar"
fi
