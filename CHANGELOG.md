# Changelog

## 0.1.1 – 2026-11-10

Fixes from the first runs on a Pi 5.

- Settings are saved on the Pi: Hatari writes hatari.cfg through a
  temporary file, and newlib's tmpfile() fails on the card (there's no
  /tmp), so hatari.cfg stayed empty, no setting was kept, and the second
  start of a card stopped with "Failed to load TOS image". Temporary files
  are now kept in memory; a card with an empty hatari.cfg gets EmuTOS again
- Saved states on a machine without TT-RAM (an ST or a Mega STE) after the
  TT-RAM setting was raised for a TT: Hatari saved TT-RAM that wasn't
  there, and the state was broken. TT-RAM is now saved as 0 there
- A state file is checked after it's saved (a failed one is deleted, and
  the menu says so) and before it's loaded: a broken one isn't loaded
  (loading one froze the Pi)
- Memory isn't used up after many changes of machine, TOS or memory, floppy
  changes and saved states: Circle never reuses a freed block over 512 KB,
  and Hatari frees and allocates the ST's memory at every cold reset
  (src/port/bigalloc.c, from BMNeXT, keeps them for reuse)
- Simulator tests for an empty hatari.cfg, a Mega STE's saved state with
  TT-RAM set, a broken state file; tools/bigalloc tests the memory reuse

## 0.1.0 – 2026-10-21

The first BMAtariST: BMApple2 0.4.2 (itself from BMCoco, BMA800XL and
BMC64), with KEGS replaced by Hatari (master after 2.6.1).

- Emulates the Atari ST, Mega ST, STE and Mega STE, and the TT and Falcon
  030 (for a Pi 4 or 5), with Hatari's own CPU, chips and timing
- EmuTOS 1.4 on the card, so it starts with nothing added; the user's own
  TOS images in the tos folder, chosen in the menu
- Floppy images (.st, .msa, .dim, .stx, .scp, zipped or gzipped), written
  back to the card two seconds after the ST writes to them, so the Pi can
  be switched off at any time; a folder on the card as drive C: (GEMDOS),
  ACSI and IDE hard disk images
- Saved states, four slots
- The ST's picture with its borders, locked to the display's refresh (50,
  60 or 71 Hz, which the Pi 4 and 5 switch the display to), with the sound
  kept in step at 44.1 or 48 kHz
- USB keyboards by key position (F11 Help, End Undo), USB mice, USB
  gamepads and GPIO joysticks in either joystick port, or the arrow keys
- A menu (F12) in the ST's font for the disks, states, machine, TOS,
  memory, monitor, joysticks, video, sound and network; the speed of the
  emulation, as a percentage of the real machine's
- The web UI starts floppy images and edits hatari.cfg; an Atari ST theme
- The PC simulator boots EmuTOS in its tests, and runs live in a browser

BMApple2's own history is in its repository:
https://github.com/jhonnaker1/bmapple2/blob/main/CHANGELOG.md
