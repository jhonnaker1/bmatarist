/*
 * menu.c - the BMAtariST menu.
 *
 * F12, a gamepad's Home button or Select+Start open it (Hatari's own F12
 * opens its settings dialog). Everything about the emulated ST (floppy and
 * hard disks, the machine, TOS, memory, joysticks) and BMAtariST's own
 * settings (video, sound, network, power).
 *
 * The ST's settings are Hatari's (ConfigureParams): they are changed here
 * and put into effect the way Hatari's own settings dialog does it
 * (bmatarist_settings_apply(), which resets the ST when a change needs it),
 * and kept in hatari.cfg. BMAtariST's are kept in bmatarist.cfg.
 *
 * From BMCoco's and BMApple2's menu.c.
 *
 * This file is part of BMAtariST, a bare metal Atari ST emulator for the
 * Raspberry Pi. See LICENSE for licensing details.
 */

#include "main.h"
#include "configuration.h"
#include "floppy.h"
#include "joy_ui.h"
#include "memorySnapShot.h"
#include "stMemory.h"
#include "tos.h"
#include "version.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#include "../host.h"
#include "bmatarist.h"
#include "osd.h"

#ifndef BMATARIST_VERSION
#define BMATARIST_VERSION "dev"
#endif

#define WPA_CONFIG "wpa_supplicant.conf"

/* Something changed that only takes effect after a reboot. */
static int reboot_needed;

/* Where the file browser was last ("" until it's first used). */
static char browse_dir[256];

/* ---- Settings ---------------------------------------------------------- */

static void save_setting(const char *name, const char *value)
{
	if (!host_option_set(name, value) || !host_options_save())
		osd_message("Cannot save bmatarist.cfg", TRUE);
}

static void save_setting_int(const char *name, int value)
{
	char text[16];
	snprintf(text, sizeof(text), "%d", value);
	save_setting(name, text);
}

static int setting_on(const char *name, int default_value)
{
	const char *value = host_option(name, NULL);
	if (value == NULL || *value == '\0')
		return default_value;
	return strcmp(value, "1") == 0 || strcmp(value, "yes") == 0
	    || strcmp(value, "true") == 0 || strcmp(value, "on") == 0;
}

static unsigned check(int on)
{
	return OSD_ITEM_CHECK | (on ? OSD_ITEM_CHECKED : 0);
}

static void power(int off)
{
	bmatarist_request_power(off);
	bmatarist_idle();
}

static const char *base_name(const char *path)
{
	const char *slash = strrchr(path, '/');
	return slash ? slash + 1 : path;
}

/* Starts the file browser in the folder last used, or DIR on the card. */
static int browse(const char *title, const char *dir, const char *const exts[],
                  char *path, size_t size)
{
	if (browse_dir[0] == '\0')
		bmatarist_card_path(browse_dir, sizeof browse_dir, dir, "");
	return osd_browse(title, browse_dir, sizeof browse_dir, exts, path, size);
}

/* ---- Floppy drives ----------------------------------------------------------- */

static const char *const floppy_exts[] = {
	"st", "msa", "dim", "stx", "scp", "zip", "gz", NULL
};

static void floppy_menu(int drive)
{
	enum { F_INSERT, F_EJECT, F_ENABLED, F_SIDES };
	int sel = 0;
	for (;;) {
		char title[24];
		const char *name = ConfigureParams.DiskImage.szDiskFileName[drive];
		int enabled = drive == 0 ? ConfigureParams.DiskImage.EnableDriveA
		                         : ConfigureParams.DiskImage.EnableDriveB;
		int heads = drive == 0 ? ConfigureParams.DiskImage.DriveA_NumberOfHeads
		                       : ConfigureParams.DiskImage.DriveB_NumberOfHeads;
		struct osd_item items[] = {
			{"Disk", name[0] ? base_name(name) : "(empty)", OSD_ITEM_LABEL, -1},
			{"", NULL, OSD_ITEM_LABEL, -1},
			{"Insert a Disk...", NULL, OSD_ITEM_SUBMENU, F_INSERT},
			{"Eject", NULL, 0, F_EJECT},
			{"Drive Connected", NULL, check(enabled), F_ENABLED},
			{"Double Sided", NULL, check(heads == 2), F_SIDES},
		};
		char path[512];
		int chosen;
		snprintf(title, sizeof title, "Floppy Drive %c:", 'A' + drive);
		chosen = osd_menu(title, items, sizeof items / sizeof items[0], &sel);
		bmatarist_settings_begin();
		switch (chosen) {
		case F_INSERT:
			if (!browse("Insert a Floppy Disk", "disks", floppy_exts, path, sizeof path))
				break;
			if (Floppy_SetDiskFileName(drive, path, NULL) == NULL) {
				osd_message("Cannot use that disk image (or it's in the other drive).", TRUE);
				break;
			}
			bmatarist_settings_apply(FALSE);
			return;
		case F_EJECT:
			Floppy_SetDiskFileNameNone(drive);
			bmatarist_settings_apply(FALSE);
			break;
		case F_ENABLED:
			if (drive == 0)
				ConfigureParams.DiskImage.EnableDriveA = !enabled;
			else
				ConfigureParams.DiskImage.EnableDriveB = !enabled;
			bmatarist_settings_apply(FALSE);
			break;
		case F_SIDES:
			if (drive == 0)
				ConfigureParams.DiskImage.DriveA_NumberOfHeads = heads == 2 ? 1 : 2;
			else
				ConfigureParams.DiskImage.DriveB_NumberOfHeads = heads == 2 ? 1 : 2;
			bmatarist_settings_apply(FALSE);
			break;
		default:
			return;
		}
		if (osd_closing())
			return;
	}
}

static void disks_menu(void)
{
	enum { D_A, D_B, D_PROTECT, D_FAST, D_AUTO_B };
	static const char *const protections[] = {"Off", "On", "Automatic", NULL};
	int sel = 0;
	for (;;) {
		const char *a = ConfigureParams.DiskImage.szDiskFileName[0];
		const char *b = ConfigureParams.DiskImage.szDiskFileName[1];
		struct osd_item items[] = {
			{"Drive A:", a[0] ? base_name(a) : "(empty)", OSD_ITEM_SUBMENU, D_A},
			{"Drive B:", b[0] ? base_name(b) : "(empty)", OSD_ITEM_SUBMENU, D_B},
			{"", NULL, OSD_ITEM_LABEL, -1},
			{"Write Protection", protections[ConfigureParams.DiskImage.nWriteProtection % 3],
			 OSD_ITEM_SUBMENU, D_PROTECT},
			{"Fast Floppy Access", NULL, check(ConfigureParams.DiskImage.FastFloppy), D_FAST},
			{"Disk 2 into B: Too", NULL, check(ConfigureParams.DiskImage.bAutoInsertDiskB),
			 D_AUTO_B},
			{"", NULL, OSD_ITEM_LABEL, -1},
			{"Disks the ST writes to are written back to the card", NULL, OSD_ITEM_LABEL, -1},
			{"two seconds later (.st, .msa, .dim).", NULL, OSD_ITEM_LABEL, -1},
		};
		int chosen = osd_menu("Floppy Disks", items, sizeof items / sizeof items[0], &sel), i;
		switch (chosen) {
		case D_A:
			floppy_menu(0);
			break;
		case D_B:
			floppy_menu(1);
			break;
		case D_PROTECT:
			i = osd_choose("Write Protection", protections,
			               ConfigureParams.DiskImage.nWriteProtection % 3);
			if (i >= 0) {
				bmatarist_settings_begin();
				ConfigureParams.DiskImage.nWriteProtection = i;
				bmatarist_settings_apply(FALSE);
			}
			break;
		case D_FAST:
			bmatarist_settings_begin();
			ConfigureParams.DiskImage.FastFloppy = !ConfigureParams.DiskImage.FastFloppy;
			bmatarist_settings_apply(FALSE);
			break;
		case D_AUTO_B:
			bmatarist_settings_begin();
			ConfigureParams.DiskImage.bAutoInsertDiskB = !ConfigureParams.DiskImage.bAutoInsertDiskB;
			bmatarist_settings_apply(FALSE);
			break;
		default:
			return;
		}
		if (osd_closing())
			return;
	}
}

/* ---- Hard disks ----------------------------------------------------------- */

static const char *const hd_exts[] = {"img", "hd", "hdf", "raw", "bin", NULL};

static void hard_disks_menu(void)
{
	enum { H_FOLDER, H_FOLDER_OFF, H_ACSI, H_ACSI_OFF, H_IDE, H_IDE_OFF, H_BOOT,
	       H_PROTECT };
	static const char *const protections[] = {"Off", "On", "Automatic", NULL};
	int sel = 0;
	for (;;) {
		const char *folder = ConfigureParams.HardDisk.szHardDiskDirectories[0];
		int use_folder = ConfigureParams.HardDisk.bUseHardDiskDirectories && folder[0];
		const char *acsi = ConfigureParams.Acsi[0].sDeviceFile;
		const char *ide = ConfigureParams.Ide[0].sDeviceFile;
		int use_acsi = ConfigureParams.Acsi[0].bUseDevice && acsi[0];
		int use_ide = ConfigureParams.Ide[0].bUseDevice && ide[0];
		struct osd_item items[] = {
			{"Folder as a Drive (GEMDOS)", use_folder ? base_name(folder) : "(none)",
			 OSD_ITEM_SUBMENU, H_FOLDER},
			{"No Folder Drive", NULL, 0, H_FOLDER_OFF},
			{"ACSI Hard Disk Image", use_acsi ? base_name(acsi) : "(none)",
			 OSD_ITEM_SUBMENU, H_ACSI},
			{"No ACSI Hard Disk", NULL, 0, H_ACSI_OFF},
			{"IDE Hard Disk Image", use_ide ? base_name(ide) : "(none)",
			 OSD_ITEM_SUBMENU, H_IDE},
			{"No IDE Hard Disk", NULL, 0, H_IDE_OFF},
			{"", NULL, OSD_ITEM_LABEL, -1},
			{"Start from the Hard Disk", NULL, check(ConfigureParams.HardDisk.bBootFromHardDisk),
			 H_BOOT},
			{"Folder Drive Write Protection",
			 protections[ConfigureParams.HardDisk.nWriteProtection % 3], OSD_ITEM_SUBMENU,
			 H_PROTECT},
			{"", NULL, OSD_ITEM_LABEL, -1},
			{"A folder on the card can be the ST's drive C: (or the", NULL, OSD_ITEM_LABEL, -1},
			{"drive after the hard disk's partitions). Changes reset", NULL, OSD_ITEM_LABEL, -1},
			{"the ST.", NULL, OSD_ITEM_LABEL, -1},
		};
		char path[FILENAME_MAX], dir[FILENAME_MAX];
		int chosen = osd_menu("Hard Disks", items, sizeof items / sizeof items[0], &sel), i;
		bmatarist_settings_begin();
		switch (chosen) {
		case H_FOLDER:
			if (use_folder)
				snprintf(dir, sizeof dir, "%s", folder);
			else
				bmatarist_card_path(dir, sizeof dir, "hd", "");
			if (!osd_browse_folder("Choose the Folder", dir, sizeof dir, path, sizeof path))
				break;
			snprintf(ConfigureParams.HardDisk.szHardDiskDirectories[0],
			         sizeof ConfigureParams.HardDisk.szHardDiskDirectories[0], "%s", path);
			ConfigureParams.HardDisk.bUseHardDiskDirectories = true;
			bmatarist_settings_apply(TRUE);
			break;
		case H_FOLDER_OFF:
			ConfigureParams.HardDisk.bUseHardDiskDirectories = false;
			bmatarist_settings_apply(TRUE);
			break;
		case H_ACSI:
		case H_IDE:
			if (!browse("Choose the Hard Disk Image", "hd", hd_exts, path, sizeof path))
				break;
			if (chosen == H_ACSI) {
				snprintf(ConfigureParams.Acsi[0].sDeviceFile,
				         sizeof ConfigureParams.Acsi[0].sDeviceFile, "%s", path);
				ConfigureParams.Acsi[0].bUseDevice = true;
			} else {
				snprintf(ConfigureParams.Ide[0].sDeviceFile,
				         sizeof ConfigureParams.Ide[0].sDeviceFile, "%s", path);
				ConfigureParams.Ide[0].bUseDevice = true;
			}
			bmatarist_settings_apply(TRUE);
			break;
		case H_ACSI_OFF:
			ConfigureParams.Acsi[0].bUseDevice = false;
			bmatarist_settings_apply(TRUE);
			break;
		case H_IDE_OFF:
			ConfigureParams.Ide[0].bUseDevice = false;
			bmatarist_settings_apply(TRUE);
			break;
		case H_BOOT:
			ConfigureParams.HardDisk.bBootFromHardDisk = !ConfigureParams.HardDisk.bBootFromHardDisk;
			bmatarist_settings_apply(FALSE);
			break;
		case H_PROTECT:
			i = osd_choose("Write Protection", protections,
			               ConfigureParams.HardDisk.nWriteProtection % 3);
			if (i >= 0) {
				ConfigureParams.HardDisk.nWriteProtection = i;
				bmatarist_settings_apply(FALSE);
			}
			break;
		default:
			return;
		}
		if (osd_closing())
			return;
	}
}

/* ---- Machine ------------------------------------------------------------------ */

static const char *const machine_names[] = {
	"Atari ST", "Mega ST", "Atari STE", "Mega STE", "Atari TT", "Falcon 030", NULL
};

/* The machine's own CPU and its speed, as Hatari's --machine sets them
   (options.c). */
static void set_machine(int m)
{
	CNF_SYSTEM *sys = &ConfigureParams.System;
	sys->nMachineType = m;
	switch (m) {
	case MACHINE_TT:
		sys->nCpuLevel = 3;
		sys->nCpuFreq = 32;
		sys->bCompatibleFPU = true;
		sys->n_FPUType = FPU_68882;
		break;
	case MACHINE_FALCON:
#if ENABLE_DSP_EMU
		sys->nDSPType = DSP_TYPE_EMU;
#endif
		sys->nCpuLevel = 3;
		sys->nCpuFreq = 16;
		sys->n_FPUType = FPU_NONE;
		break;
	default:
		sys->nCpuLevel = 0;
		sys->nCpuFreq = 8;
		sys->n_FPUType = FPU_NONE;
		sys->bMMU = false;
		sys->bAddressSpace24 = true;
		break;
	}
	/* A colour monitor for the machine (the Falcon's VGA, an ST's RGB),
	   unless the monochrome one was chosen. */
	if (ConfigureParams.Screen.nMonitorType != MONITOR_TYPE_MONO)
		ConfigureParams.Screen.nMonitorType = m == MACHINE_FALCON || m == MACHINE_TT
		                                      ? MONITOR_TYPE_VGA : MONITOR_TYPE_RGB;
}

static const char *tos_value(void)
{
	int n;
	for (n = 0; n < bmatarist_tos_count(); n++)
		if (strcmp(bmatarist_tos_path(n), ConfigureParams.Rom.szTosImageFileName) == 0)
			return bmatarist_tos_name(n);
	return base_name(ConfigureParams.Rom.szTosImageFileName);
}

static const int st_ram[] = {512, 1024, 2048, 2560, 4096, 8192, 14336};

static const char *ram_text(int kb, char *buf, size_t size)
{
	if (kb < 1024)
		snprintf(buf, size, "%d KB", kb);
	else if (kb % 1024)
		snprintf(buf, size, "%d.%d MB", kb / 1024, kb % 1024 * 10 / 1024);
	else
		snprintf(buf, size, "%d MB", kb / 1024);
	return buf;
}

static void machine_menu(void)
{
	enum { S_MACHINE, S_TOS, S_RAM, S_TTRAM, S_MONITOR, S_BLITTER, S_SPEED, S_EXACT,
	       S_FASTBOOT };
	static const char *const monitors[] = {"Monochrome (SM124)", "Colour (SC1224)", "VGA",
	                                       "TV", NULL};
	static const char *const speeds[] = {"8 MHz", "16 MHz", "32 MHz", NULL};
	int sel = 0;
	for (;;) {
		CNF_SYSTEM *sys = &ConfigureParams.System;
		char ram[16], ttram[16], speed[12];
		int tt_or_falcon = sys->nMachineType == MACHINE_TT
		                   || sys->nMachineType == MACHINE_FALCON;
		struct osd_item items[] = {
			{"Machine", machine_names[sys->nMachineType % 6], OSD_ITEM_SUBMENU, S_MACHINE},
			{"TOS", tos_value(), OSD_ITEM_SUBMENU, S_TOS},
			{"Memory", ram_text(ConfigureParams.Memory.STRamSize_KB, ram, sizeof ram),
			 OSD_ITEM_SUBMENU, S_RAM},
			{"TT-RAM (Fast RAM)", ram_text(ConfigureParams.Memory.TTRamSize_KB, ttram, sizeof ttram),
			 OSD_ITEM_SUBMENU, S_TTRAM},
			{"Monitor", monitors[ConfigureParams.Screen.nMonitorType % 4], OSD_ITEM_SUBMENU,
			 S_MONITOR},
			{"Blitter (ST)", NULL, check(sys->bBlitter || sys->nMachineType != MACHINE_ST),
			 S_BLITTER},
			{"CPU Speed", (snprintf(speed, sizeof speed, "%d MHz", sys->nCpuFreq), speed),
			 OSD_ITEM_SUBMENU, S_SPEED},
			{"Cycle Exact CPU", NULL, check(sys->bCycleExactCpu), S_EXACT},
			{"Fast Boot (skip TOS's memory test)", NULL, check(sys->bFastBoot), S_FASTBOOT},
			{"", NULL, OSD_ITEM_LABEL, -1},
			{"Changes here reset the ST. The TT and the Falcon need", NULL, OSD_ITEM_LABEL, -1},
			{"a Raspberry Pi 4 or 5, and a TOS of their own (or EmuTOS).", NULL, OSD_ITEM_LABEL, -1},
		};
		const char *tos_names[33];
		const char *ram_names[8];
		char ram_bufs[7][16];
		int chosen, i, n;
		(void) tt_or_falcon;
		chosen = osd_menu("Machine", items, sizeof items / sizeof items[0], &sel);
		bmatarist_settings_begin();
		switch (chosen) {
		case S_MACHINE:
			i = osd_choose("Machine", machine_names, sys->nMachineType % 6);
			if (i >= 0 && i != (int) sys->nMachineType) {
				set_machine(i);
				bmatarist_settings_apply(TRUE);
			}
			break;
		case S_TOS:
			n = bmatarist_tos_count();
			if (n == 0) {
				osd_message("There are no TOS images in the tos folder.", TRUE);
				break;
			}
			if (n > 32)
				n = 32;
			for (i = 0; i < n; i++)
				tos_names[i] = bmatarist_tos_name(i);
			tos_names[n] = NULL;
			i = osd_choose("TOS", tos_names, 0);
			if (i >= 0) {
				snprintf(ConfigureParams.Rom.szTosImageFileName,
				         sizeof ConfigureParams.Rom.szTosImageFileName, "%s",
				         bmatarist_tos_path(i));
				bmatarist_settings_apply(TRUE);
			}
			break;
		case S_RAM:
			for (i = 0; i < 7; i++)
				ram_names[i] = ram_text(st_ram[i], ram_bufs[i], sizeof ram_bufs[i]);
			ram_names[7] = NULL;
			i = osd_choose("Memory", ram_names, 1);
			if (i >= 0) {
				ConfigureParams.Memory.STRamSize_KB = st_ram[i];
				bmatarist_settings_apply(TRUE);
			}
			break;
		case S_TTRAM: {
			static const char *const tt_names[] = {"None", "16 MB", "32 MB", "64 MB",
			                                       "128 MB", "256 MB", NULL};
			static const int tt_kb[] = {0, 16384, 32768, 65536, 131072, 262144};
			i = osd_choose("TT-RAM (TT and Falcon)", tt_names, 0);
			if (i >= 0) {
				ConfigureParams.Memory.TTRamSize_KB = tt_kb[i];
				bmatarist_settings_apply(TRUE);
			}
			break;
		}
		case S_MONITOR:
			i = osd_choose("Monitor", monitors, ConfigureParams.Screen.nMonitorType % 4);
			if (i >= 0) {
				ConfigureParams.Screen.nMonitorType = i;
				bmatarist_settings_apply(FALSE);
			}
			break;
		case S_BLITTER:
			sys->bBlitter = !sys->bBlitter;
			bmatarist_settings_apply(FALSE);
			break;
		case S_SPEED:
			i = osd_choose("CPU Speed", speeds, sys->nCpuFreq >= 32 ? 2 : sys->nCpuFreq >= 16);
			if (i >= 0) {
				sys->nCpuFreq = 8 << i;
				bmatarist_settings_apply(FALSE);
			}
			break;
		case S_EXACT:
			sys->bCycleExactCpu = !sys->bCycleExactCpu;
			bmatarist_settings_apply(FALSE);
			break;
		case S_FASTBOOT:
			sys->bFastBoot = !sys->bFastBoot;
			bmatarist_settings_apply(FALSE);
			break;
		default:
			return;
		}
		if (osd_closing())
			return;
	}
}

/* ---- Joysticks ----------------------------------------------------------- */

/* What can be in an ST joystick port: nothing, one of the host joysticks
   (joy_ui.c), or the arrow keys. */
static const char *const joy_choices[] = {
	"None", "USB Gamepad 1", "USB Gamepad 2", "GPIO Joystick 1", "GPIO Joystick 2",
	"Arrow Keys (Right Ctrl fires)", NULL
};

static int joy_choice(int port)
{
	JOYSTICK *j = &ConfigureParams.Joysticks.Joy[port];
	if (j->nJoystickMode == JOYSTICK_KEYBOARD)
		return 5;
	if (j->nJoystickMode == JOYSTICK_REALSTICK && j->nJoyId >= 0 && j->nJoyId < 4)
		return 1 + j->nJoyId;
	return 0;
}

static void set_joy(int port, int choice)
{
	JOYSTICK *j = &ConfigureParams.Joysticks.Joy[port];
	if (choice == 0) {
		j->nJoystickMode = JOYSTICK_DISABLED;
	} else if (choice == 5) {
		j->nJoystickMode = JOYSTICK_KEYBOARD;
		JoyUI_SetDefaultKeys(port);
	} else {
		j->nJoystickMode = JOYSTICK_REALSTICK;
		j->nJoyId = choice - 1;
	}
}

static void joysticks_menu(void)
{
	enum { J_PORT1, J_PORT0, J_AUTOFIRE, J_JUMP };
	int sel = 0;
	for (;;) {
		JOYSTICK *j1 = &ConfigureParams.Joysticks.Joy[1];
		struct osd_item items[] = {
			{"Joystick Port", joy_choices[joy_choice(1)], OSD_ITEM_SUBMENU, J_PORT1},
			{"Mouse Port", joy_choices[joy_choice(0)], OSD_ITEM_SUBMENU, J_PORT0},
			{"Autofire (joystick port)", NULL, check(j1->bEnableAutoFire), J_AUTOFIRE},
			{"B Button Presses Space", NULL, check(j1->bEnableJumpOnFire2), J_JUMP},
			{"", NULL, OSD_ITEM_LABEL, -1},
			{"Most games use the joystick port. With a joystick in", NULL, OSD_ITEM_LABEL, -1},
			{"the mouse port, the USB mouse isn't the ST's.", NULL, OSD_ITEM_LABEL, -1},
			{"", NULL, OSD_ITEM_LABEL, -1},
			{"Gamepads: A fires, B is the second button, X types", NULL, OSD_ITEM_LABEL, -1},
			{"Space and Y Return; the shoulder buttons autofire.", NULL, OSD_ITEM_LABEL, -1},
		};
		int chosen = osd_menu("Joysticks", items, sizeof items / sizeof items[0], &sel), i;
		bmatarist_settings_begin();
		switch (chosen) {
		case J_PORT1:
		case J_PORT0: {
			int port = chosen == J_PORT1 ? 1 : 0;
			i = osd_choose(port ? "Joystick Port" : "Mouse Port", joy_choices, joy_choice(port));
			if (i >= 0) {
				set_joy(port, i);
				bmatarist_settings_apply(FALSE);
			}
			break;
		}
		case J_AUTOFIRE:
			j1->bEnableAutoFire = !j1->bEnableAutoFire;
			bmatarist_settings_apply(FALSE);
			break;
		case J_JUMP:
			j1->bEnableJumpOnFire2 = !j1->bEnableJumpOnFire2;
			bmatarist_settings_apply(FALSE);
			break;
		default:
			return;
		}
		if (osd_closing())
			return;
	}
}

/* ---- Video -------------------------------------------------------------- */

static const char *vsync_mode(void)
{
	const char *mode = host_option("vsync", "auto");
	if (strcmp(mode, "on") == 0 || strcmp(mode, "1") == 0)
		return "On";
	if (strcmp(mode, "off") == 0 || strcmp(mode, "0") == 0)
		return "Off";
	return bmatarist_vsync_locked() ? "Auto (locked)" : "Auto (timer)";
}

/* The picture's height as a percentage of the screen's (the scale option):
   less for a TV that cuts off the edges (overscan). From BMCoco. */
static int screen_size(void)
{
	double scale = host_option_double("scale", 1.0);
	if (scale <= 0.1 || scale > 1.0)
		scale = 1.0;
	return (int) (scale * 100.0 + 0.5);
}

static void choose_screen_size(void)
{
	static const char *const choices[] = {
		"100%: fill the screen", "95%", "90%", "85%", "80%", NULL
	};
	static const int sizes[] = {100, 95, 90, 85, 80};
	int size = screen_size(), current = 0, k, i;
	char text[16];
	for (k = 1; k < 5; k++) {
		int d = sizes[k] - size, best = sizes[current] - size;
		if ((d < 0 ? -d : d) < (best < 0 ? -best : best))
			current = k;
	}
	i = osd_choose("Screen Size", choices, current);
	if (i < 0)
		return;
	snprintf(text, sizeof text, "%d.%02d", sizes[i] / 100, sizes[i] % 100);
	save_setting("scale", text);
	bmatarist_video_settings_changed();
}

static void video_menu(void)
{
	enum { V_SIZE, V_VSYNC, V_RESOLUTION, V_SHADER, V_CURVATURE, V_SMOOTH,
	       V_ACTIVITY };
	/* The Pi 5 only: its firmware ignores hdmi_mode (from BMA800XL). */
	static const char *const resolutions[] = {"auto", "1920x1080", "1280x720", NULL};
	static const char *const resolution_names[] = {"Auto", "1920x1080", "1280x720", NULL};
	int sel = 0;
	for (;;) {
		const char *res = host_option("resolution", "auto");
		int res_index = 0, n_items, k;
		char size_text[8];
		snprintf(size_text, sizeof size_text, "%d%%", screen_size());
		for (k = 0; resolutions[k] != NULL; k++)
			if (strcmp(res, resolutions[k]) == 0)
				res_index = k;
		struct osd_item items[] = {
			{"Screen Size", size_text, OSD_ITEM_SUBMENU, V_SIZE},
			{"Frame Lock", vsync_mode(), OSD_ITEM_SUBMENU, V_VSYNC},
			{"Resolution", resolution_names[res_index], OSD_ITEM_SUBMENU, V_RESOLUTION},
			{"CRT Effect (reboot)", NULL, check(setting_on("crt_shader", FALSE)), V_SHADER},
			{"CRT Curvature (reboot)", NULL, check(setting_on("crt_curvature", FALSE)), V_CURVATURE},
			{"Smooth Scaling (reboot)", NULL, check(setting_on("smooth", FALSE)), V_SMOOTH},
			{"Show Disk Activity", NULL, check(bmatarist_disk_activity()), V_ACTIVITY},
			{"", NULL, OSD_ITEM_LABEL, -1},
			{"If your TV cuts off the edges, make", NULL, OSD_ITEM_LABEL, -1},
			{"the Screen Size smaller.", NULL, OSD_ITEM_LABEL, -1},
		};
		int chosen, i;
		n_items = sizeof(items) / sizeof(items[0]);
		if (!host_display_resolution_choice()) {
			/* Resolution is only for the Pi 5 (with video_driver=kms). */
			for (k = 2; k < n_items - 1; k++)
				items[k] = items[k + 1];
			n_items--;
		}
		chosen = osd_menu("Video", items, n_items, &sel);
		switch (chosen) {
		case V_SIZE:
			choose_screen_size();
			break;
		case V_ACTIVITY:
			save_setting("disk_activity", bmatarist_disk_activity() ? "0" : "1");
			bmatarist_set_disk_activity(!bmatarist_disk_activity());
			break;
		case V_RESOLUTION:
			i = osd_choose("Resolution", resolution_names, res_index);
			if (i >= 0 && i != res_index) {
				save_setting("resolution", resolutions[i]);
				bmatarist_display_mode_changed();
			}
			break;
		case V_CURVATURE:
			save_setting("crt_curvature", setting_on("crt_curvature", FALSE) ? "0" : "1");
			reboot_needed = TRUE;
			break;
		case V_VSYNC: {
			static const char *const choices[] = {
				"Automatic", "On: follow the display", "Off: keep exact time", NULL
			};
			static const char *const values[] = {"auto", "on", "off"};
			const char *mode = host_option("vsync", "auto");
			int current = strcmp(mode, "on") == 0 ? 1 : strcmp(mode, "off") == 0 ? 2 : 0;
			i = osd_choose("Frame Lock", choices, current);
			if (i >= 0) {
				save_setting("vsync", values[i]);
				bmatarist_video_settings_changed();
			}
			break;
		}
		case V_SHADER:
			save_setting("crt_shader", setting_on("crt_shader", FALSE) ? "0" : "1");
			reboot_needed = TRUE;
			break;
		case V_SMOOTH:
			save_setting("smooth", setting_on("smooth", FALSE) ? "0" : "1");
			reboot_needed = TRUE;
			break;
		default:
			return;
		}
	}
}

/* ---- Sound ---------------------------------------------------------------- */

static void sound_menu(void)
{
	enum { S_VOLUME, S_OUTPUT };
	static const char *const volumes[] = {
		"100%", "90%", "80%", "70%", "60%", "50%", "40%", "30%", "20%", "10%", "Off", NULL
	};
	static const char *const outputs[] = {"auto", "hdmi", "analog"};
	static const char *const output_names[] = {"Automatic", "HDMI", "Headphone jack", NULL};
	int sel = 0;
	for (;;) {
		const char *out = host_option("audio_out", "auto");
		int out_index = strcmp(out, "hdmi") == 0 ? 1
		                : strcmp(out, "analog") == 0 || strcmp(out, "headphones") == 0 ? 2 : 0;
		char volume[8];
		struct osd_item items[] = {
			{"Volume", volume, OSD_ITEM_SUBMENU, S_VOLUME},
			{"Output (reboot)", output_names[out_index], OSD_ITEM_SUBMENU, S_OUTPUT},
		};
		int chosen, i;
		if (bmatarist_volume() == 0)
			snprintf(volume, sizeof volume, "Off");
		else
			snprintf(volume, sizeof volume, "%d%%", bmatarist_volume());
		chosen = osd_menu("Sound", items, sizeof(items) / sizeof(items[0]), &sel);
		if (chosen == S_VOLUME) {
			i = osd_choose("Volume", volumes, (100 - bmatarist_volume() + 5) / 10);
			if (i >= 0)
				bmatarist_set_volume(100 - i * 10);
			continue;
		}
		if (chosen != S_OUTPUT)
			return;
		i = osd_choose("Sound Output", output_names, out_index);
		if (i >= 0 && i != out_index) {
			save_setting("audio_out", outputs[i]);
			reboot_needed = TRUE;
		}
	}
}

/* ---- Network --------------------------------------------------------------- */

static char wifi_ssid[33], wifi_psk[64], wifi_country[3];

static void wpa_path(char *path, size_t size)
{
	const char *root = bmatarist_sd_root();
	size_t len = strlen(root);
	snprintf(path, size, "%s%s%s", root, len > 0 && root[len - 1] == '/' ? "" : "/", WPA_CONFIG);
}

static void copy_quoted(char *dst, size_t size, const char *src)
{
	const char *end;
	if (*src == '"')
		src++;
	end = strchr(src, '"');
	if (end == NULL)
		end = src + strcspn(src, "\r\n");
	if ((size_t) (end - src) >= size)
		end = src + size - 1;
	memcpy(dst, src, end - src);
	dst[end - src] = '\0';
}

static void read_wifi_config(void)
{
	char path[256], line[160];
	FILE *f;
	wifi_ssid[0] = wifi_psk[0] = '\0';
	strcpy(wifi_country, "US");
	wpa_path(path, sizeof(path));
	f = fopen(path, "r");
	if (f == NULL)
		return;
	while (fgets(line, sizeof(line), f) != NULL) {
		char *p = line + strspn(line, " \t");
		if (strncmp(p, "country=", 8) == 0)
			copy_quoted(wifi_country, sizeof(wifi_country), p + 8);
		else if (strncmp(p, "ssid=", 5) == 0 && wifi_ssid[0] == '\0')
			copy_quoted(wifi_ssid, sizeof(wifi_ssid), p + 5);
		else if (strncmp(p, "psk=", 4) == 0 && wifi_psk[0] == '\0')
			copy_quoted(wifi_psk, sizeof(wifi_psk), p + 4);
	}
	fclose(f);
}

static int write_wifi_config(void)
{
	char path[256];
	FILE *f;
	wpa_path(path, sizeof(path));
	f = fopen(path, "w");
	if (f == NULL)
		return FALSE;
	fprintf(f, "country=%s\n\nnetwork={\n\tssid=\"%s\"\n", wifi_country, wifi_ssid);
	if (wifi_psk[0] != '\0')
		fprintf(f, "\tpsk=\"%s\"\n\tkey_mgmt=WPA-PSK\n", wifi_psk);
	else
		fprintf(f, "\tkey_mgmt=NONE\n");
	fprintf(f, "}\n");
	return fclose(f) == 0;
}

static void edit_wifi(const char *title, char *value, size_t size, int secret)
{
	char text[64];
	snprintf(text, sizeof(text), "%s", value);
	if (osd_edit(title, text, size < sizeof(text) ? size : sizeof(text), secret)) {
		snprintf(value, size, "%s", text);
		if (write_wifi_config())
			reboot_needed = TRUE;
		else
			osd_message("Cannot write " WPA_CONFIG, TRUE);
	}
}

static int compare_signal(const void *a, const void *b)
{
	return ((const struct host_wifi_ap *) b)->signal - ((const struct host_wifi_ap *) a)->signal;
}

/* Returns 1 if a network was chosen, 2 if it also needs a password. */
static int scan_wifi(void)
{
	enum { MAX_APS = 20 };
	static struct host_wifi_ap aps[MAX_APS];
	static char signal[MAX_APS][16];
	struct osd_item items[MAX_APS];
	int n, i, sel = 0, chosen;

	osd_message("Scanning for Wi-Fi networks...", FALSE);
	n = host_wifi_scan(aps, MAX_APS);
	if (n < 0) {
		if (osd_confirm("Wi-Fi is Off", "Switch to Wi-Fi and reboot?")) {
			save_setting("network", "wifi");
			power(0);
		}
		return 0;
	}
	if (n == 0) {
		osd_message("No Wi-Fi networks found.", TRUE);
		return 0;
	}
	qsort(aps, n, sizeof(aps[0]), compare_signal);
	for (i = 0; i < n; i++) {
		snprintf(signal[i], sizeof(signal[i]), "%s%d", aps[i].secure ? "* " : "", aps[i].signal);
		items[i] = (struct osd_item){aps[i].ssid, signal[i], 0, i};
	}
	chosen = osd_menu("Choose a Wi-Fi Network", items, n, &sel);
	if (chosen < 0)
		return 0;
	if (!aps[chosen].secure || strcmp(wifi_ssid, aps[chosen].ssid) != 0)
		wifi_psk[0] = '\0';
	snprintf(wifi_ssid, sizeof(wifi_ssid), "%s", aps[chosen].ssid);
	if (!write_wifi_config()) {
		osd_message("Cannot write " WPA_CONFIG, TRUE);
		return 0;
	}
	save_setting("network", "wifi");
	reboot_needed = TRUE;
	return aps[chosen].secure && wifi_psk[0] == '\0' ? 2 : 1;
}

static void network_menu(void)
{
	enum { N_DEVICE, N_SCAN, N_SSID, N_PASSWORD, N_COUNTRY, N_WEBUI, N_PIN, N_TIMEZONE, N_REBOOT };
	static const char *const devices[] = {"off", "ethernet", "wifi", NULL};
	static const char *const device_names[] = {"Off", "Ethernet", "Wi-Fi", NULL};
	int sel = 2;
	for (;;) {
		char status[64], tz[16];
		const char *net = host_option("network", "off");
		const char *pin = host_option("webui_pin", "");
		int device = strcmp(net, "ethernet") == 0 ? 1 : strcmp(net, "wifi") == 0 ? 2 : 0;
		int minutes = host_option_int("timezone", 0);
		int chosen, i;

		read_wifi_config();
		host_network_status(status, sizeof(status));
		if (minutes == 0)
			strcpy(tz, "UTC");
		else
			snprintf(tz, sizeof(tz), "UTC%+d", minutes / 60);
		{
			struct osd_item items[] = {
				{status, NULL, OSD_ITEM_LABEL, -1},
				{"", NULL, OSD_ITEM_LABEL, -1},
				{"Network", device_names[device], OSD_ITEM_SUBMENU, N_DEVICE},
				{"Scan for Wi-Fi Networks", NULL, OSD_ITEM_SUBMENU, N_SCAN},
				{"Wi-Fi Network", wifi_ssid[0] ? wifi_ssid : "(none)", OSD_ITEM_SUBMENU, N_SSID},
				{"Wi-Fi Password", wifi_psk[0] ? "********" : "(none)", OSD_ITEM_SUBMENU, N_PASSWORD},
				{"Wi-Fi Country", wifi_country, OSD_ITEM_SUBMENU, N_COUNTRY},
				{"Web UI", NULL, check(setting_on("webui", TRUE)), N_WEBUI},
				{"Web UI PIN", pin[0] ? pin : "(none)", OSD_ITEM_SUBMENU, N_PIN},
				{"Time Zone", tz, OSD_ITEM_SUBMENU, N_TIMEZONE},
				{"", NULL, OSD_ITEM_LABEL, -1},
				{"Changes apply after a reboot.", NULL, OSD_ITEM_LABEL, -1},
				{"Reboot Now", NULL, 0, N_REBOOT},
			};
			chosen = osd_menu("Network", items, sizeof(items) / sizeof(items[0]), &sel);
		}
		switch (chosen) {
		case N_DEVICE:
			i = osd_choose("Network", device_names, device);
			if (i >= 0 && i != device) {
				save_setting("network", devices[i]);
				reboot_needed = TRUE;
			}
			break;
		case N_SCAN:
			if (scan_wifi() == 2) {
				wifi_psk[0] = '\0';
				edit_wifi("Wi-Fi Password", wifi_psk, sizeof(wifi_psk), TRUE);
			}
			break;
		case N_SSID:
			edit_wifi("Wi-Fi Network Name", wifi_ssid, sizeof(wifi_ssid), FALSE);
			break;
		case N_PASSWORD:
			wifi_psk[0] = '\0';
			edit_wifi("Wi-Fi Password (blank: none)", wifi_psk, sizeof(wifi_psk), TRUE);
			break;
		case N_COUNTRY:
			edit_wifi("Two Letter Country Code", wifi_country, sizeof(wifi_country), FALSE);
			break;
		case N_WEBUI:
			save_setting("webui", setting_on("webui", TRUE) ? "0" : "1");
			reboot_needed = TRUE;
			break;
		case N_PIN: {
			char text[9];
			snprintf(text, sizeof(text), "%s", pin);
			if (osd_edit("Web UI PIN (blank: none)", text, sizeof(text), FALSE)) {
				save_setting("webui_pin", text);
				reboot_needed = TRUE;
			}
			break;
		}
		case N_TIMEZONE: {
			static char labels[27][8];
			static const char *zones[28];
			int h, current = 12;
			for (h = -12; h <= 14; h++) {
				if (h == 0)
					strcpy(labels[h + 12], "UTC");
				else
					snprintf(labels[h + 12], sizeof(labels[0]), "UTC%+d", h);
				zones[h + 12] = labels[h + 12];
				if (h * 60 == minutes)
					current = h + 12;
			}
			zones[27] = NULL;
			h = osd_choose("Time Zone", zones, current);
			if (h >= 0 && h != current) {
				save_setting_int("timezone", (h - 12) * 60);
				reboot_needed = TRUE;
			}
			break;
		}
		case N_REBOOT:
			power(0);
			return;
		default:
			return;
		}
	}
}

/* ---- Saved states --------------------------------------------------------- */

/* Four slots, snapshots/state1.sav to state4.sav on the card: Hatari's own
   memory snapshots (the whole ST: memory, chips, CPU, disks in the drives).
   A restore is done by the CPU core when it starts again (platform.c's main
   loop restarts it, as after a reset). */
#define STATE_SLOTS 4

static void state_path(int slot, char *path, size_t size)
{
	char name[16];
	snprintf(name, sizeof name, "state%d.sav", slot + 1);
	bmatarist_card_path(path, size, "snapshots", name);
}

/* "Slot 2: 2026-10-20 19:31", or "Slot 2: (empty)". */
static void state_label(int slot, char *text, size_t size)
{
	char path[256];
	struct stat st;
	state_path(slot, path, sizeof path);
	if (stat(path, &st) == 0) {
		struct tm *t = localtime(&st.st_mtime);
		if (t != NULL)
			snprintf(text, size, "Slot %d: %04d-%02d-%02d %02d:%02d", slot + 1,
			         t->tm_year + 1900, t->tm_mon + 1, t->tm_mday, t->tm_hour, t->tm_min);
		else
			snprintf(text, size, "Slot %d: saved", slot + 1);
	} else {
		snprintf(text, size, "Slot %d: (empty)", slot + 1);
	}
}

/* Is the state file whole? Hatari's state is a gzip stream that ends with
   SNAPSHOT_MAGIC (0xDeadBeef, memorySnapShot.c). A save that failed half
   way leaves a broken file, and restoring one leaves the ST half restored:
   Hatari stops at an assert() (it froze on the Pi). */
static int state_file_ok(const char *path)
{
	static const unsigned char magic[4] = {0xef, 0xbe, 0xad, 0xde};
	unsigned char last[4] = {0, 0, 0, 0};
	unsigned char *buf;
	long total = 0;
	int n, err, ok;
	gzFile f = gzopen(path, "rb");
	if (f == NULL)
		return FALSE;
	buf = malloc(64 * 1024);
	if (buf == NULL) {
		gzclose(f);
		return FALSE;
	}
	while ((n = gzread(f, buf, 64 * 1024)) > 0) {
		if (n >= 4) {
			memcpy(last, buf + n - 4, 4);
		} else {
			memmove(last, last + n, 4 - n);
			memcpy(last + 4 - n, buf, n);
		}
		total += n;
	}
	gzerror(f, &err);
	ok = n == 0 && err == Z_OK && total >= 4 && memcmp(last, magic, 4) == 0;
	/* Z_BUF_ERROR: the file ended inside the gzip stream. */
	if (gzclose(f) != Z_OK)
		ok = FALSE;
	free(buf);
	return ok;
}

/* Returns TRUE if a state was loaded (the menu then closes). */
static int states_menu(int load)
{
	char labels[STATE_SLOTS][48];
	const char *choices[STATE_SLOTS + 1];
	char path[256], dir[256];
	struct stat st;
	int i;
	for (i = 0; i < STATE_SLOTS; i++) {
		state_label(i, labels[i], sizeof labels[i]);
		choices[i] = labels[i];
	}
	choices[STATE_SLOTS] = NULL;
	i = osd_choose(load ? "Load State" : "Save State", choices, 0);
	if (i < 0)
		return FALSE;
	state_path(i, path, sizeof path);
	if (load) {
		if (stat(path, &st) != 0) {
			osd_message("That slot is empty.", TRUE);
			return FALSE;
		}
		if (!state_file_ok(path)) {
			printf("BMAtariST: %s is broken, not loaded\n", path);
			osd_message("That saved state is broken (an old save that failed), so it wasn't loaded.", TRUE);
			return FALSE;
		}
		printf("BMAtariST: loading %s\n", path);
		MemorySnapShot_Restore(path, false);
		return TRUE;
	}
	bmatarist_card_path(dir, sizeof dir, "snapshots", "");
	mkdir(dir, 0777);
	printf("BMAtariST: saving %s\n", path);
	{
		/* Hatari saves TT-RAM whenever the setting isn't 0 (stMemory.c),
		   but a machine without it (an ST, or a Mega STE after a TT) has
		   none to save: the save failed. Saved, and so restored, as 0. */
		int tt_kb = ConfigureParams.Memory.TTRamSize_KB;
		if (TTmemory == NULL)
			ConfigureParams.Memory.TTRamSize_KB = 0;
		MemorySnapShot_Capture_Immediate(path, false);
		ConfigureParams.Memory.TTRamSize_KB = tt_kb;
	}
	if (stat(path, &st) != 0 || !state_file_ok(path)) {
		printf("BMAtariST: saving %s failed\n", path);
		unlink(path);
		osd_message("Cannot save the state on the SD card.", TRUE);
	} else {
		osd_notify("State saved");
	}
	return FALSE;
}

/* ---- Information ----------------------------------------------------------- */

static void about(void)
{
	char text[900], status[64];
	int w, h;
	host_display_size(&w, &h);
	host_network_status(status, sizeof(status));
	snprintf(text, sizeof(text),
	         "BMAtariST " BMATARIST_VERSION "\n"
	         "Bare metal Atari ST, STE, TT and Falcon emulator for the\n"
	         "Raspberry Pi.\n\n"
	         "Emulation: " PROG_NAME " (GPL 2 or later), by the Hatari team.\n"
	         "TOS: EmuTOS (GPL 2), or your own.\n"
	         "Based on BMApple2, BMCoco, BMA800XL and BMC64.\n\n"
	         "Machine: %s\n"
	         "TOS: %s\n"
	         "Speed: %d%% of the real machine's\n"
	         "Display: %dx%d %.2f Hz, frames %s\n"
	         "Network: %s",
	         machine_names[ConfigureParams.System.nMachineType % 6], tos_value(),
	         bmatarist_speed_percent(), w, h, bmatarist_refresh_rate(),
	         bmatarist_vsync_locked() ? "locked to the display" : "timer paced", status);
	osd_info("About BMAtariST", text);
}

static void offer_reboot(void)
{
	if (osd_confirm("Reboot?", "Settings changed that need a reboot. Reboot now?"))
		power(0);
}

/* ---- Main menu ---------------------------------------------------------------- */

void bmatarist_menu(void)
{
	enum { M_DISKS, M_HD, M_SAVE, M_LOAD, M_RESET, M_RESTART, M_MACHINE, M_JOYSTICKS,
	       M_VIDEO, M_SOUND, M_NETWORK, M_ABOUT, M_REBOOT, M_OFF };
	static int sel = 0;
	char title[48], speed[48];

	snprintf(title, sizeof(title), "BMAtariST %s", BMATARIST_VERSION);
	reboot_needed = FALSE;
	osd_open();
	for (;;) {
		const char *a = ConfigureParams.DiskImage.szDiskFileName[0];
		struct osd_item items[] = {
			{"Floppy Disks", a[0] ? base_name(a) : NULL, OSD_ITEM_SUBMENU, M_DISKS},
			{"Hard Disks", NULL, OSD_ITEM_SUBMENU, M_HD},
			{"Save State", NULL, OSD_ITEM_SUBMENU, M_SAVE},
			{"Load State", NULL, OSD_ITEM_SUBMENU, M_LOAD},
			{"Reset (the ST's reset button)", NULL, 0, M_RESET},
			{"Restart (switch the ST off and on)", NULL, 0, M_RESTART},
			{"", NULL, OSD_ITEM_LABEL, -1},
			{"Machine", machine_names[ConfigureParams.System.nMachineType % 6],
			 OSD_ITEM_SUBMENU, M_MACHINE},
			{"Joysticks", NULL, OSD_ITEM_SUBMENU, M_JOYSTICKS},
			{"Video", NULL, OSD_ITEM_SUBMENU, M_VIDEO},
			{"Sound", NULL, OSD_ITEM_SUBMENU, M_SOUND},
			{"Network", NULL, OSD_ITEM_SUBMENU, M_NETWORK},
			{"About", NULL, OSD_ITEM_SUBMENU, M_ABOUT},
			{"", NULL, OSD_ITEM_LABEL, -1},
			{"Reboot", NULL, 0, M_REBOOT},
			{"Shut Down", NULL, 0, M_OFF},
			{"", NULL, OSD_ITEM_LABEL, -1},
			{speed, NULL, OSD_ITEM_LABEL, -1},
		};
		int chosen, leave = FALSE;
		snprintf(speed, sizeof speed, "Running at %d%% of the real machine's speed.",
		         bmatarist_speed_percent());
		chosen = osd_menu(title, items, sizeof(items) / sizeof(items[0]), &sel);

		switch (chosen) {
		case M_DISKS: disks_menu(); break;
		case M_HD: hard_disks_menu(); break;
		case M_SAVE: states_menu(FALSE); leave = TRUE; break;
		case M_LOAD: leave = states_menu(TRUE); break;
		case M_RESET: bmatarist_warm_reset(); leave = TRUE; break;
		case M_RESTART: bmatarist_cold_reset(); leave = TRUE; break;
		case M_MACHINE: machine_menu(); break;
		case M_JOYSTICKS: joysticks_menu(); break;
		case M_VIDEO: video_menu(); break;
		case M_SOUND: sound_menu(); break;
		case M_NETWORK: network_menu(); break;
		case M_ABOUT: about(); break;
		case M_REBOOT:
			if (osd_confirm("Reboot", "Reboot BMAtariST?"))
				power(0);
			break;
		case M_OFF:
			if (osd_confirm("Shut Down", "Shut down? It's then safe to switch off."))
				power(1);
			break;
		default:
			leave = TRUE;
			break;
		}
		if (leave || osd_closing() || bmatarist_pending_requests())
			break;
	}
	if (reboot_needed && !osd_closing())
		offer_reboot();
	osd_close();
}
