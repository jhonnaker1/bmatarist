#!/bin/bash
#
# Runs the emulator port on the PC (see host_sim.c) through a set of
# scripted tests and checks the results.
#
#   tools/hostsim/run_tests.sh
#
# The tests boot EmuTOS (fetched by make_emutos.sh, as for the SD card) to
# its desktop. Output, including screenshots (.ppm), goes to
# tools/hostsim/out/.
#

set -u

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT=$HERE/out
SIM=$HERE/build/bmatarist_sim

make -s -C "$HERE" -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)" || exit 1

rm -rf "$OUT"
mkdir -p "$OUT/card"
"$ROOT/make_emutos.sh" "$OUT/card" > /dev/null || exit 1
TOS=$OUT/card/tos

FAILED=0
pass() { echo "PASS  $1"; }
fail() { echo "FAIL  $1: $2"; FAILED=$((FAILED + 1)); }

# run NAME REFRESH CMDLINE SCRIPT: runs the simulator in out/NAME, a
# stand-in SD card with EmuTOS in its tos folder.
run() {
    local name=$1 refresh=$2 cmdline=$3 script=$4
    mkdir -p "$OUT/$name"
    ln -s "$TOS" "$OUT/$name/tos" 2>/dev/null
    (cd "$OUT/$name" && printf "$script" | BMATARIST_REFRESH=$refresh \
        BMATARIST_CMDLINE="root=. $cmdline" "$SIM" > log.txt 2>&1)
}

# check_log NAME REGEX MESSAGE: the log must contain REGEX
check_log() {
    if grep -Eq "$2" "$OUT/$1/log.txt"; then
        pass "$1: $3"
    else
        fail "$1" "$3 (see out/$1/log.txt)"
    fi
}

# check_nolog NAME REGEX MESSAGE: out/NAME/log.txt must not have REGEX
check_nolog() {
    if grep -Eq "$2" "$OUT/$1/log.txt"; then
        fail "$1" "$3 (see out/$1/log.txt)"
    else
        pass "$1: $3"
    fi
}

# check_cfg NAME FILE REGEX MESSAGE: out/NAME/FILE must have a line REGEX
check_cfg() {
    if grep -Eq "$3" "$OUT/$1/$2" 2>/dev/null; then
        pass "$1: $4"
    else
        fail "$1" "$4 (not in out/$1/$2)"
    fi
}

no_underruns() {
    if grep -q "audio underrun" "$OUT/$1/log.txt"; then
        fail "$1" "audio underrun (see out/$1/log.txt)"
    else
        pass "$1: no audio underruns"
    fi
}

# every_frame NAME: every display refresh showed a frame
every_frame() {
    local line frame presented
    line=$(grep "frames presented" "$OUT/$1/log.txt" | tail -1)
    frame=$(echo "$line" | sed 's/.*frame \([0-9]*\):.*/\1/')
    presented=$(echo "$line" | sed 's/.*: \([0-9]*\) frames presented/\1/')
    if [ -n "$frame" ] && [ "${presented:-0}" -ge "$frame" ]; then
        pass "$1: no dropped frames ($presented/$frame)"
    else
        fail "$1" "frames presented: $line"
    fi
}

# sound_made NAME: something other than silence reached the output
sound_made() {
    local changes
    changes=$(grep "changing samples" "$OUT/$1/log.txt" | tail -1 | sed 's/.*changing samples \([0-9]*\).*/\1/')
    if [ "${changes:-0}" -gt 0 ]; then
        pass "$1: sound output"
    else
        fail "$1" "no sound reached the output"
    fi
}

# count NAME FILE R G B: how many pixels of that colour the screenshot has
count() {
    python3 - "$OUT/$1/$2.ppm" "$3" "$4" "$5" <<'EOF'
import sys
try:
    data = open(sys.argv[1], 'rb').read()
except OSError:
    print(0)
    sys.exit(0)
parts = data.split(b'\n', 3)
px = parts[3]
want = bytes(int(v) for v in sys.argv[2:5])
print(sum(1 for i in range(0, len(px), 3) if px[i:i + 3] == want))
EOF
}

# desktop NAME FILE: EmuTOS's green desktop is on the screen (an ST's
# green, 0x070, or an STE's, 0x0F0)
desktop() {
    local n
    n=$(( $(count "$1" "$2" 0 238 0) + $(count "$1" "$2" 0 255 0) ))
    if [ "$n" -gt 50000 ]; then
        pass "$1: EmuTOS's desktop ($2)"
    else
        fail "$1" "no desktop in $2.ppm ($n green pixels)"
    fi
}

same() {
    if [ -f "$OUT/$1/$2.ppm" ] && [ -f "$OUT/$1/$3.ppm" ] && cmp -s "$OUT/$1/$2.ppm" "$OUT/$1/$3.ppm"; then
        pass "$1: $2 and $3 are the same"
    else
        fail "$1" "$2 and $3 differ"
    fi
}
differ() {
    if [ -f "$OUT/$1/$2.ppm" ] && [ -f "$OUT/$1/$3.ppm" ] && ! cmp -s "$OUT/$1/$2.ppm" "$OUT/$1/$3.ppm"; then
        pass "$1: $2 and $3 differ"
    else
        fail "$1" "$2 and $3 should differ"
    fi
}

# USB HID usages used below.
F12=0x45 DOWN=0x51 ENTER=0x28 ESC=0x29 POWER=0x66 A=0x04

# KEY USAGE FRAME: press at FRAME, release two frames later
key() { printf '%d key %s 1\\n%d key %s 0\\n' "$2" "$1" $(($2 + 2)) "$1"; }
# DOWNS N FRAME: N presses of Down from FRAME, four frames apart
downs() {
    local i
    for ((i = 0; i < $1; i++)); do key $DOWN $(($2 + i * 4)); done
}

echo "Boot to EmuTOS's desktop, locked to a 60 Hz display"
# The 1024K EmuTOS is a US one: an NTSC ST, 60 Hz.
run boot 60 "" '1500 shot a.ppm\n1501 shot b.ppm\n1800 audio\n1801 quit\n'
check_log boot "ST 60\.0[0-9] Hz, synchronised to the display" "locked to 60 Hz"
check_log boot "1 TOS image" "EmuTOS found in tos/"
check_log boot "picture 832x552" "the ST's picture with its borders"
check_cfg boot hatari.cfg "^szTosImageFileName = .*etos1024k\.img" "a new card's hatari.cfg names EmuTOS"
desktop boot a
same boot a b
no_underruns boot
every_frame boot

echo "Starting again from the saved settings"
mkdir -p "$OUT/boot2"
cp "$OUT/boot/hatari.cfg" "$OUT/boot2/hatari.cfg"
run boot2 60 "" '1500 shot a.ppm\n1501 quit\n'
desktop boot2 a

echo "Starting with an empty hatari.cfg (0.1.0 left one on the Pi)"
mkdir -p "$OUT/emptycfg"
: > "$OUT/emptycfg/hatari.cfg"
run emptycfg 60 "" '1500 shot a.ppm\n1501 quit\n'
check_log emptycfg "the TOS in hatari.cfg isn't there" "an empty hatari.cfg gets EmuTOS"
check_cfg emptycfg hatari.cfg "^szTosImageFileName = .*etos1024k\.img" "and it is saved in hatari.cfg"
desktop emptycfg a

echo "On a 50 Hz display"
run timer 50 "" '1500 shot a.ppm\n1800 audio\n1801 quit\n'
check_log timer "timer paced" "a 60 Hz ST on a 50 Hz display is timer paced"
desktop timer a
no_underruns timer

echo "Sound at 44.1 and 48 kHz: the keyboard's click"
# Keys pressed on the desktop click (the YM2149). The ST's 60.04 Hz locked
# to 60 Hz runs a little fast: the sound must be squeezed to fit, never
# cut off with the buffer full.
for rate in 44100 48000; do
    BMATARIST_SIM_RATE=$rate run sound$rate 60 "" \
        "$(key $A 1500)$(key $A 1560)$(key $A 1620)3000 audio\n3001 quit\n"
    line=$(grep "audio written" "$OUT/sound$rate/log.txt" | tail -1)
    max=$(echo "$line" | sed 's/.*after write max \([0-9]*\).*/\1/')
    cap=$(grep "sound open" "$OUT/sound$rate/log.txt" | head -1 | sed 's/.*, \([0-9]*\) frames.*/\1/')
    if [ -n "$max" ] && [ -n "$cap" ] && [ "$max" -le $((cap * 90 / 100)) ]; then
        pass "sound$rate: the sound buffer never fills ($max of $cap)"
    else
        fail sound$rate "the sound buffer filled: $line (capacity $cap)"
    fi
    no_underruns sound$rate
    sound_made sound$rate
done
run volume0 60 "volume=0" "$(key $A 1500)$(key $A 1560)1800 audio\n1801 quit\n"
check_log volume0 "changing samples 0" "volume=0 is silent"

echo "Mouse"
# Moving the mouse moves the ST's pointer; the right button doesn't stop
# anything.
run mouse 60 "" '1500 shot before.ppm\n1510 mouse 40 30 0\n1511 mouse 40 30 0\n1512 mouse 40 30 0\n1530 shot after.ppm\n1540 mouse 0 0 2\n1542 mouse 0 0 0\n1560 shot right.ppm\n1561 quit\n'
differ mouse before after
desktop mouse right

echo "Menu (F12)"
# Open, Esc closes; the picture comes back as it was.
run menu 60 "" "1500 shot before.ppm\n$(key $F12 1510)1530 shot menu.ppm\n$(key $ESC 1540)1560 shot after.ppm\n1561 quit\n"
n=$(count menu menu 255 255 255)
if [ "$n" -gt 100000 ]; then
    pass "menu: F12 opens the menu"
else
    fail menu "no menu on the screen ($n white pixels)"
fi
same menu before after
run padmenu 60 "" '1500 pad 0 0\n1510 pad 0 0x0100\n1514 pad 0 0\n1530 shot menu.ppm\n1531 quit\n'
n=$(count padmenu menu 255 255 255)
if [ "$n" -gt 100000 ]; then
    pass "padmenu: a gamepad's Home opens the menu"
else
    fail padmenu "no menu on the screen ($n white pixels)"
fi

echo "Settings are saved"
# Menu, Video (eight down), Show Disk Activity (five down): off.
run settings 60 "" "$(key $F12 1500)$(downs 8 1510)$(key $ENTER 1550)$(downs 5 1560)$(key $ENTER 1590)$(key $F12 1600)1620 quit\n"
check_cfg settings bmatarist.cfg "^disk_activity=0" "Video > Show Disk Activity saved to bmatarist.cfg"

echo "Machine: switching to an STE"
# Menu, Machine (six down), Machine, two down to the STE. Hatari resets
# the ST and saves the choice in hatari.cfg.
run machine 60 "" "$(key $F12 1500)$(downs 6 1510)$(key $ENTER 1540)$(key $ENTER 1550)$(downs 2 1560)$(key $ENTER 1570)$(key $ESC 1580)$(key $ESC 1590)3200 shot ste.ppm\n3201 quit\n"
check_cfg machine hatari.cfg "^nModelType = 2" "the STE saved in hatari.cfg"
desktop machine ste

echo "Save and load a state"
# Menu, Save State (two down), slot 1; the pointer moves; menu (it opens on
# Save State again), Load State (one down), slot 1: the screen is as saved
# (once the "State saved" note has gone).
run states 60 "" "1500 shot saved.ppm\n$(key $F12 1510)$(downs 2 1520)$(key $ENTER 1535)$(key $ENTER 1545)1600 mouse 60 40 0\n1601 mouse 60 40 0\n1620 shot moved.ppm\n$(key $F12 1630)$(downs 1 1640)$(key $ENTER 1650)$(key $ENTER 1660)1900 shot loaded.ppm\n1901 quit\n"
check_log states "Memory state file saved: .*snapshots/state1\.sav" "saved to snapshots/state1.sav"
check_log states "Memory state file restored" "loaded"
differ states saved moved
same states saved loaded

echo "Save and load a state: a Mega STE, with TT-RAM set from a TT before"
# A machine without TT-RAM keeps the TT-RAM setting a TT had: Hatari would
# save the TT-RAM it doesn't have, and the state failed (a broken file).
mkdir -p "$OUT/ttstates"
cp "$OUT/boot/hatari.cfg" "$OUT/ttstates/hatari.cfg"
python3 - "$OUT/ttstates/hatari.cfg" <<'EOF'
import sys
cfg = sys.argv[1]
lines = open(cfg).read().split('\n')
set_to = {'nMemorySize': '14336', 'nTTRamSize': '262144', 'nModelType': '3'}
lines = [(l.split(' =')[0] + ' = ' + set_to[l.split(' =')[0]]) if l.split(' =')[0] in set_to else l
         for l in lines]
open(cfg, 'w').write('\n'.join(lines))
EOF
run ttstates 60 "" "1500 shot saved.ppm\n$(key $F12 1510)$(downs 2 1520)$(key $ENTER 1535)$(key $ENTER 1545)1600 mouse 60 40 0\n1601 mouse 60 40 0\n1620 shot moved.ppm\n$(key $F12 1630)$(downs 1 1640)$(key $ENTER 1650)$(key $ENTER 1660)1900 shot loaded.ppm\n1901 quit\n"
check_log ttstates "Memory state file saved: .*snapshots/state1\.sav" "saved"
check_log ttstates "Memory state file restored" "loaded"
differ ttstates saved moved
same ttstates saved loaded

echo "A broken state file: not loaded, the ST goes on"
mkdir -p "$OUT/badstate/snapshots"
cp "$OUT/boot/hatari.cfg" "$OUT/badstate/hatari.cfg"
head -c 100000 "$OUT/states/snapshots/state1.sav" > "$OUT/badstate/snapshots/state1.sav"
run badstate 60 "" "1500 shot a.ppm\n$(key $F12 1510)$(downs 3 1520)$(key $ENTER 1535)$(key $ENTER 1545)1600 shot msg.ppm\n$(key $ENTER 1610)$(key $ESC 1630)1700 shot b.ppm\n1701 quit\n"
check_log badstate "state1\.sav is broken, not loaded" "a broken state isn't loaded"
check_nolog badstate "Memory state file restored" "nothing restored"
desktop badstate b

echo "A folder as drive C:"
mkdir -p "$OUT/gemdos/hd/C"
echo "hello" > "$OUT/gemdos/hd/C/HELLO.TXT"
cp "$OUT/boot/hatari.cfg" "$OUT/gemdos/hatari.cfg"
python3 - "$OUT/gemdos/hatari.cfg" "$OUT/gemdos/hd/C" <<'EOF'
import sys
cfg, folder = sys.argv[1], sys.argv[2]
s = open(cfg).read()
s = s.replace('bUseHardDiskDirectory = FALSE', 'bUseHardDiskDirectory = TRUE')
lines = [('szHardDiskDirectory = ' + folder) if l.startswith('szHardDiskDirectory =') else l
         for l in s.split('\n')]
open(cfg, 'w').write('\n'.join(lines))
EOF
run gemdos 60 "" '1500 shot c.ppm\n1501 quit\n'
cp "$OUT/boot/a.ppm" "$OUT/gemdos/noc.ppm"
differ gemdos noc c
check_log gemdos "GEMDOS HDD emulation, C: <-> " "Hatari's GEMDOS drive C:"

echo "Web UI requests"
run reboot 60 "" '1500 webui reboot\n1560 quit\n'
check_log reboot "reboot requested" "reboots"
run coldstart 60 "" '1500 webui coldstart\n1560 quit\n'
check_log coldstart "Web UI: restart" "restarts the ST"
python3 -c "open('$OUT/blank.st','wb').write(bytes(737280))"
run start 60 "" "1500 run $OUT/blank.st\n1600 quit\n"
check_log start "Web UI: starting .*blank\.st" "starts a floppy image"
check_cfg start hatari.cfg "^szDiskAFileName = .*blank\.st" "it is in drive A:"

echo "The power key (Pi 400: Fn+F10) shuts down"
run powerkey 60 "" "$(key $POWER 1500)1560 quit\n"
check_log powerkey "Shutting down" "the power key while the ST runs"
check_log powerkey "power off requested" "the power key powers off"

echo
if [ $FAILED -eq 0 ]; then
    echo "All tests passed."
else
    echo "$FAILED test(s) failed."
fi
exit $((FAILED > 0))
