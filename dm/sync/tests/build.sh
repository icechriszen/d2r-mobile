#!/bin/bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${DM_SYNC_TEST_OUT:?Set an external test output directory}
mkdir -p "$OUT"
clang -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -Werror -Wall -Wextra \
  -Wno-deprecated-declarations -Wno-unused-parameter -fobjc-arc -DDM_SYNC_TEST_DRIVER \
  "$ROOT/sync/dm_snapshot.c" "$ROOT/sync/dm_sync.c" "$ROOT/sync/dm_sync_ui.c" "$ROOT/sync/vendor/cJSON.c" \
  "$ROOT/sync/cloud_platform_apple.m" "$ROOT/sync/tests/driver.c" \
  -framework Foundation -framework Security -framework AppKit -o "$OUT/sync-driver"
