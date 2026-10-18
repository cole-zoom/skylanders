#!/usr/bin/env bash
# Drive Skylanders into actual gameplay headlessly and capture a FIFO log.
#
# Getting past the title screen needs three things the title-screen capture
# did not: a figure on the emulated portal, a Nunchuk attached, and scripted
# button input. Without any one of them the game stops at a prompt.
#
# Usage: scripts/record-gameplay.sh <out.dff> [fifo-delay-seconds]
set -euo pipefail
cd "$(dirname "$0")/.."

OUT="${1:-harness/traces/ssa-gameplay.dff}"
DELAY="${2:-470}"
USERDIR=/tmp/dolphin-user
BIN=dolphin-web/build-native/Binaries/dolphin-emu-nogui
ROM=$(ls skylanders-rom/*.rvz 2>/dev/null | head -1)

[ -x "$BIN" ] || { echo "oracle not built; run scripts/build-oracle.sh" >&2; exit 1; }
[ -n "$ROM" ] || { echo "no disc image in skylanders-rom/" >&2; exit 1; }

mkdir -p "$USERDIR/Pipes" "$USERDIR/Config" "$(dirname "$OUT")"

# Scripted input goes through Dolphin's Pipes device (InputCommon/
# ControllerInterface/Pipes). It reads "PRESS A" / "RELEASE A" / "SET MAIN X v"
# lines from a FIFO. Button token names come from s_button_tokens in Pipes.cpp.
rm -f "$USERDIR/Pipes/ctl"
mkfifo "$USERDIR/Pipes/ctl"

# The Nunchuk is required: the game blocks on "Player 1: Connect Nunchuk to
# the Wii Remote". The attachment key is plain `Extension`, not
# `Extension/Attached Extension`.
cat > "$USERDIR/Config/WiimoteNew.ini" <<'EOF'
[Wiimote1]
Source = 1
Device = Pipe/0/ctl
Buttons/A = `Button A`
Buttons/B = `Button B`
Buttons/1 = `Button X`
Buttons/2 = `Button Y`
Buttons/- = `Button L`
Buttons/+ = `Button START`
Buttons/Home = `Button R`
D-Pad/Up = `Button D_UP`
D-Pad/Down = `Button D_DOWN`
D-Pad/Left = `Button D_LEFT`
D-Pad/Right = `Button D_RIGHT`
Extension = Nunchuk
Nunchuk/Buttons/C = `Button Z`
Nunchuk/Buttons/Z = `Button R`
Nunchuk/Stick/Up = `Axis MAIN Y +`
Nunchuk/Stick/Down = `Axis MAIN Y -`
Nunchuk/Stick/Left = `Axis MAIN X -`
Nunchuk/Stick/Right = `Axis MAIN X +`
Nunchuk/Stick/Dead Zone = 15.0
EOF

# The emulated portal must be on before boot, when USBScanner enumerates.
python3 - "$USERDIR/Config/Dolphin.ini" <<'EOF'
import sys
p = sys.argv[1]
try: s = open(p).read()
except FileNotFoundError: s = ''
if 'EmulateSkylanderPortal' not in s:
    s += "\n[EmulatedUSBDevices]\nEmulateSkylanderPortal = True\n"
open(p, 'w').write(s)
EOF

# -v Null: the FIFO recorder hooks the command stream, not the renderer, so no
# GPU is needed. It also avoids a reproducible Metal crash during level load
# in headless mode ("Failed to allocate IOGPUDeviceShmem").
echo "==> booting (figure at 60s, capture at ${DELAY}s)"
"$BIN" -u "$USERDIR" -p headless -v Null -a HLE \
  --skylander=16:0 --skylander-delay=60 \
  --fifo-record=3 --fifo-delay="$DELAY" --fifo-output="$PWD/$OUT" \
  -e "$ROM" >/tmp/record-gameplay.log 2>&1 &
DOLPHIN=$!

# Hold the pipe open: a write-and-close reads as EOF to Dolphin.
(
  exec 3> "$USERDIR/Pipes/ctl"
  sleep 75
  for _ in $(seq 1 120); do
    printf 'PRESS A\n' >&3;  sleep 0.12; printf 'RELEASE A\n' >&3; sleep 2
    printf 'PRESS B\n' >&3;  sleep 0.12; printf 'RELEASE B\n' >&3; sleep 2
    printf 'SET MAIN X 0.95\n' >&3; sleep 1.2
    printf 'SET MAIN X 0.5\n'  >&3; sleep 0.3
  done
) &
INPUT=$!

trap 'kill $DOLPHIN $INPUT 2>/dev/null || true' EXIT
while kill -0 $DOLPHIN 2>/dev/null && [ ! -f "$OUT" ]; do sleep 10; done
sleep 5
kill $INPUT 2>/dev/null || true

if [ -f "$OUT" ]; then
  echo "==> captured $OUT ($(stat -f%z "$OUT") bytes)"
  grep -iE '^\[(fifo|sky)\]' /tmp/record-gameplay.log || true
else
  echo "capture failed; see /tmp/record-gameplay.log" >&2
  exit 1
fi
