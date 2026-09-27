#!/bin/bash
# Build the engine (mach build) into $GECKO_W10M_OBJ. See tools/env.sh for the
# locations and how to override them.
source "$(dirname "$0")/env.sh" || exit 1
cd "$GECKO_W10M_ROOT/engine/firefox"
gecko_w10m_stage_intrin
MOZ_BUILD_DATE="$(gecko_w10m_build_date)"
export MOZ_BUILD_DATE
echo "=== browser BUILD start $(date), MOZ_BUILD_DATE=$MOZ_BUILD_DATE ==="
# Most of the machine, not all of it: the build should not page itself to death.
export CARGO_BUILD_JOBS="${CARGO_BUILD_JOBS:-$(( GECKO_W10M_JOBS * 2 / 3 ))}"
./mach build -j"$GECKO_W10M_JOBS" < /dev/null 2>&1
status=$?
echo "=== build exit=$status at $(date) ==="
exit $status
