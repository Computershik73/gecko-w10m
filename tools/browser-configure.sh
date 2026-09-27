#!/bin/bash
# Configure the engine (mach configure) into $GECKO_W10M_OBJ. See tools/env.sh
# for the locations and how to override them.
source "$(dirname "$0")/env.sh" || exit 1
cd "$GECKO_W10M_ROOT/engine/firefox"
gecko_w10m_stage_intrin
echo "=== browser configure start $(date) ==="
./mach configure < /dev/null 2>&1
status=$?
echo "=== configure exit=$status at $(date) ==="
exit $status
