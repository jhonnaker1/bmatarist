/*
 * platform.c - Hatari platform layer for BMAtariST.
 *
 * Does what Hatari's front ends (src/sdl/main_sdl.c, src/retro/main_retro.c)
 * do, on top of the Circle kernel services in src/host.h: starts Hatari,
 * runs its CPU a frame (one ST VBL) at a time, copies its picture to the
 * display, and keeps its floppy images written back to the SD card. The
 * keyboard, mouse and gamepads are read in gui_event.c, the joysticks in
 * joy_ui.c, the sound is in audio.c, the picture in screen.c, the menu in
 * menu.c and osd.c.
 *
 * Frames: Hatari's CPU core stops at each VBL (timing.c, as in its libretro
 * core); when the display refreshes at (close to) the ST's rate (50 Hz PAL,
 * 60 Hz NTSC, 71 Hz on the monochrome monitor) each frame is shown on its
 * own display refresh, so scrolling is perfectly smooth; otherwise a timer
 * keeps the pace.
 *
 * Settings: Hatari's own (machine, TOS, memory, disks, joysticks...) are
 * kept in hatari.cfg in the card's root, which Hatari reads and writes
 * itself; BMAtariST's (video, sound volume, network) in bmatarist.cfg.
 *
 * This file is part of BMAtariST, a bare metal Atari ST emulator for the
 * Raspberry Pi. See LICENSE for licensing details.
 */

#include "main.h"
#include "configuration.h"
#include "change.h"
#include "clocks_timings.h"
#include "dialog.h"
#include "file.h"
#include "floppy.h"
#include "floppies/dim.h"
#include "floppies/msa.h"
#include "floppies/st.h"
#include "m68000.h"
#include "reset.h"
#include "screen.h"
#include "sound.h"
#include "tos.h"
#include "video.h"
#include "hatari-glue.h"
#include "newcpu.h"

#include <ctype.h>
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../host.h"
#include "bmatarist.h"
#include "osd.h"

/* ---- Settings ---------------------------------------------------------------- */

const char *bmatarist_sd_root(void)
{
	return host_option("root", "/");
}

void bmatarist_card_path(char *path, unsigned size, const char *dir, const char *name)
{
	const char *root = bmatarist_sd_root();
	size_t len = strlen(root);
	snprintf(path, size, "%s%s%s%s%s", root, len > 0 && root[len - 1] == '/' ? "" : "/",
	         dir, dir[0] ? "/" : "", name);
}

static int option_on(const char *name, int default_value)
{
	const char *value = host_option(name, NULL);
	if (value == NULL || *value == '\0')
		return default_value;
	return strcmp(value, "1") == 0 || strcmp(value, "on") == 0
	    || strcmp(value, "yes") == 0 || strcmp(value, "true") == 0;
}

static char hatari_cfg[256];

/* ---- Video ------------------------------------------------------------------- */

static uint16_t *fb_pixels;
static int fb_pitch_px;
static int osd_view;

static double refresh_rate = 50.0;
static int vsync_lock;
static double machine_rate;     /* the ST's frame rate the mode was set for */

uint16_t *bmatarist_framebuffer(int *pitch_px)
{
	*pitch_px = fb_pitch_px;
	return fb_pixels;
}

static void apply_view(void)
{
	int w, h;
	double aspect;
	if (osd_view) {
		host_video_set_view(0, 0, 640, 460, 640.0 / 460.0);
		return;
	}
	bmatarist_screen_view(&w, &h, &aspect);
	host_video_set_view(0, 0, w, h, aspect * host_option_double("aspect", 1.0));
}

void bmatarist_osd_view(int on)
{
	osd_view = on;
	apply_view();
	if (!on)
		bmatarist_screen_copy(TRUE);
}

static void clear_framebuffer(void)
{
	int y;
	for (y = 0; y < BMATARIST_FB_H; y++)
		memset(fb_pixels + y * fb_pitch_px, 0, BMATARIST_FB_W * 2);
}

/* The ST's frame rate: 50.05 Hz (PAL), 60.04 Hz (NTSC) or 71.2 Hz (the
   monochrome monitor), a little different on each machine (Hatari's
   clocks_timings.c). */
static double st_rate(void)
{
	uint32_t vbl = ClocksTimings_GetVBLPerSec(ConfigureParams.System.nMachineType,
	                                          nScreenRefreshRate);
	double rate = (double) vbl / (1 << CLOCKS_TIMINGS_SHIFT_VBL);
	return rate > 20.0 && rate < 100.0 ? rate : 50.0;
}

static void measure_refresh_rate(void)
{
	enum { SKIP = 3, COUNT = 20 };
	double start = 0.0, rate;
	int i;
	for (i = 0; i < SKIP + COUNT; i++) {
		host_video_present(TRUE);
		if (i == SKIP - 1)
			start = host_time();
	}
	rate = COUNT / (host_time() - start);
	if (rate > 20.0 && rate < 250.0)
		refresh_rate = rate;
	printf("BMAtariST: display refresh rate %.3f Hz\n", refresh_rate);
}

static void update_sync_mode(void)
{
	const char *mode = host_option("vsync", "auto");
	double rate = st_rate();
	if (strcmp(mode, "off") == 0 || strcmp(mode, "0") == 0)
		vsync_lock = FALSE;
	else if (strcmp(mode, "on") == 0 || strcmp(mode, "1") == 0)
		vsync_lock = TRUE;
	else {
		double ratio = refresh_rate / rate;
		vsync_lock = ratio > 0.98 && ratio < 1.02;
	}
	printf("BMAtariST: display %.2f Hz, ST %.2f Hz, %s\n", refresh_rate, rate,
	       vsync_lock ? "synchronised to the display" : "timer paced");
}

/* Where the host can set the video mode (the Pi 4 and 5), the display is
   set to the ST's rate, and timed again, whenever that changes (PAL, NTSC
   or the monochrome monitor). */
static void match_display_rate(void)
{
	double rate = st_rate();
	if (rate == machine_rate)
		return;
	machine_rate = rate;
	if (host_display_match_rate(rate))
		measure_refresh_rate();
	update_sync_mode();
}

double bmatarist_refresh_rate(void)
{
	return refresh_rate;
}

int bmatarist_vsync_locked(void)
{
	return vsync_lock;
}

void bmatarist_video_settings_changed(void)
{
	if (fb_pixels == NULL)
		return;
	update_sync_mode();
	clear_framebuffer();
	apply_view();
}

void bmatarist_display_mode_changed(void)
{
	machine_rate = 0.0;
	match_display_rate();
	bmatarist_sound_open();
	apply_view();
}

/* ---- Machine ------------------------------------------------------------- */

/* "Atari STE", for the web UI and the menu. */
const char *bmatarist_machine_text(void)
{
	static const char *const names[] = {
		"Atari ST", "Mega ST", "Atari STE", "Mega STE", "Atari TT", "Falcon 030"
	};
	return names[ConfigureParams.System.nMachineType % 6];
}

/* ---- Speed --------------------------------------------------------------- */

static int speed_percent = 100;

int bmatarist_speed_percent(void)
{
	return speed_percent;
}

/* ---- Disk activity -------------------------------------------------------- */

static int da_enabled = -1;

int bmatarist_disk_activity(void)
{
	if (da_enabled < 0)
		da_enabled = host_option_int("disk_activity", 1) != 0;
	return da_enabled;
}

void bmatarist_set_disk_activity(int on)
{
	da_enabled = on != 0;
}

/* ---- Files ---------------------------------------------------------------- */

/* Hatari keeps a floppy image in memory and writes it back to the card when
   it's ejected. The Pi can be switched off at any time, so a floppy the ST
   has written to is written back once its drive has been still for two
   seconds, and everything is pushed out to the card once a second. */
static double floppy_changed_at[MAX_FLOPPYDRIVES];

static void write_back_floppies(double now)
{
	int d;
	for (d = 0; d < MAX_FLOPPYDRIVES; d++) {
		EMULATION_DRIVE *drv = &EmulationDrives[d];
		const char *name = drv->sFileName;
		bool saved = false;
		if (!drv->bDiskInserted || !drv->bContentsChanged || !drv->bOKToSave) {
			floppy_changed_at[d] = 0.0;
			continue;
		}
		/* bContentsChanged stays set: it's written again two seconds
		   after the last change of each burst of writing. */
		if (floppy_changed_at[d] == 0.0) {
			floppy_changed_at[d] = now;
			continue;
		}
		if (now - floppy_changed_at[d] < 2.0)
			continue;
		if (MSA_FileNameIsMSA(name, true))
			saved = MSA_WriteDisk(d, name, drv->pBuffer, drv->nImageBytes);
		else if (ST_FileNameIsST(name, true))
			saved = ST_WriteDisk(d, name, drv->pBuffer, drv->nImageBytes);
		else if (DIM_FileNameIsDIM(name, true))
			saved = DIM_WriteDisk(d, name, drv->pBuffer, drv->nImageBytes);
		else
			continue;   /* the rest are written when ejected, as Hatari does */
		printf("BMAtariST: %s %s\n", saved ? "wrote back" : "cannot write back", name);
		drv->bContentsChanged = false;
		floppy_changed_at[d] = 0.0;
	}
}

static void sync_files(void)
{
	int fd;
	fflush(NULL);
	for (fd = 3; fd < 30; fd++)
		fsync(fd);
}

/* ---- TOS ---------------------------------------------------------------- */

/* The TOS images on the card: the tos folder, then roms (and the root),
   any file ending .img, .rom or .tos of a size a TOS can be (192K for TOS
   1.0x, 256K for 1.62 and 2.0x, 512K for TOS 3 and 4 and the smaller
   EmuTOS, 1024K for the EmuTOS that runs on every machine). EmuTOS comes
   on the card, so there's always one. */
#define MAX_TOS 32

static char tos_paths[MAX_TOS][256];
static char tos_names[MAX_TOS][64];
static int tos_count;

static int tos_size_ok(long size)
{
	return size == 192 * 1024 || size == 256 * 1024 || size == 512 * 1024
	    || size == 1024 * 1024;
}

static int has_tos_ext(const char *name)
{
	const char *dot = strrchr(name, '.');
	return dot != NULL && (strcasecmp(dot, ".img") == 0 || strcasecmp(dot, ".rom") == 0
	                       || strcasecmp(dot, ".tos") == 0);
}

/* "TOS 1.04 (UK)" or "EmuTOS (etos1024k.img)", from the image's header. */
static void describe_tos(const char *path, char *name, size_t size)
{
	static const char *const countries[] = {
		"US", "DE", "FR", "UK", "ES", "IT", "SE", "CH-FR", "CH-DE", "TR", "FI",
		"NO", "DK", "SA", "NL", "CZ", "HU", "PL", "LT", "RU", "EE", "BY", "UA",
		"SK", "RO", "BG", "SI", "HR", "RS", "ME", "MK", "GR", "LV", "IL", "ZA",
		"PT", "BE", "JP", "CN", "KR", "VN", "IN", "IR", "MN", "NP", "LA", "KH",
		"ID", "BD"
	};
	unsigned char head[0x40];
	const char *base = strrchr(path, '/');
	FILE *f = fopen(path, "rb");
	size_t got = 0;
	base = base ? base + 1 : path;
	if (f != NULL) {
		got = fread(head, 1, sizeof head, f);
		fclose(f);
	}
	if (got == sizeof head && memcmp(head + 0x2c, "ETOS", 4) == 0) {
		snprintf(name, size, "EmuTOS (%.40s)", base);
	} else if (got == sizeof head) {
		unsigned country = (head[0x1d] >> 1) & 0x7f;
		snprintf(name, size, "TOS %x.%02x %s (%.40s)", head[2], head[3],
		         country < sizeof countries / sizeof countries[0] ? countries[country] : "",
		         base);
	} else {
		snprintf(name, size, "%.60s", base);
	}
}

static void find_tos(void)
{
	static const char *const dirs[] = {"tos", "roms", ""};
	unsigned d;
	tos_count = 0;
	for (d = 0; d < sizeof dirs / sizeof dirs[0]; d++) {
		char dir_path[256];
		DIR *dir;
		struct dirent *e;
		bmatarist_card_path(dir_path, sizeof dir_path, dirs[d], "");
		dir = opendir(dir_path);
		if (dir == NULL)
			continue;
		while ((e = readdir(dir)) != NULL && tos_count < MAX_TOS) {
			char path[256];
			struct stat st;
			if (e->d_name[0] == '.' || !has_tos_ext(e->d_name))
				continue;
			bmatarist_card_path(path, sizeof path, dirs[d], e->d_name);
			if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || !tos_size_ok(st.st_size))
				continue;
			snprintf(tos_paths[tos_count], sizeof tos_paths[0], "%s", path);
			describe_tos(path, tos_names[tos_count], sizeof tos_names[0]);
			tos_count++;
		}
		closedir(dir);
	}
	/* Sorted by name, EmuTOS first. */
	{
		int i, j;
		for (i = 1; i < tos_count; i++) {
			for (j = i; j > 0 && strcmp(tos_names[j - 1], tos_names[j]) > 0; j--) {
				char p[256], n[64];
				memcpy(p, tos_paths[j], sizeof p);
				memcpy(n, tos_names[j], sizeof n);
				memcpy(tos_paths[j], tos_paths[j - 1], sizeof p);
				memcpy(tos_names[j], tos_names[j - 1], sizeof n);
				memcpy(tos_paths[j - 1], p, sizeof p);
				memcpy(tos_names[j - 1], n, sizeof n);
			}
		}
	}
	printf("BMAtariST: %d TOS image(s)\n", tos_count);
}

int bmatarist_tos_count(void)
{
	return tos_count;
}

const char *bmatarist_tos_path(int n)
{
	return n >= 0 && n < tos_count ? tos_paths[n] : "";
}

const char *bmatarist_tos_name(int n)
{
	return n >= 0 && n < tos_count ? tos_names[n] : "";
}

/* The TOS to start with: the one in hatari.cfg if it's there, else the
   1024K EmuTOS (it runs on every machine), else the first found. */
static const char *default_tos(void)
{
	int i;
	for (i = 0; i < tos_count; i++)
		if (strstr(tos_paths[i], "etos1024") != NULL)
			return tos_paths[i];
	return tos_count > 0 ? tos_paths[0] : NULL;
}

/* Is the TOS image hatari.cfg names there? Read from the file itself: it
   is checked before Hatari has started. A hatari.cfg that names none (an
   empty one: 0.1.0 couldn't save its settings on the Pi) counts as not. */
static int cfg_tos_exists(const char *cfg)
{
	char line[512];
	int found = FALSE;
	FILE *f = fopen(cfg, "r");
	if (f == NULL)
		return FALSE;
	while (fgets(line, sizeof line, f) != NULL) {
		char *p = line + strspn(line, " \t");
		struct stat st;
		if (strncmp(p, "szTosImageFileName", 18) != 0)
			continue;
		p = strchr(p, '=');
		if (p == NULL)
			break;
		p += 1 + strspn(p + 1, " \t");
		p[strcspn(p, "\r\n")] = '\0';
		found = *p != '\0' && stat(p, &st) == 0;
		break;
	}
	fclose(f);
	return found;
}

/* ---- Settings changes ----------------------------------------------------- */

static CNF_PARAMS settings_before;

void bmatarist_settings_begin(void)
{
	settings_before = ConfigureParams;
}

void bmatarist_settings_apply(int force_reset)
{
	/* Hatari's own settings dialog's way (dialog.c): it works out what
	   has to be reset or reloaded for the changes. */
	Change_CopyChangedParamsToConfiguration(&settings_before, &ConfigureParams,
	                                        force_reset);
	Configuration_Save();
	settings_before = ConfigureParams;
	bmatarist_video_settings_changed();
	machine_rate = 0.0;
}

/* ---- Resets ------------------------------------------------------------------- */

void bmatarist_cold_reset(void)
{
	Reset_Cold();
}

void bmatarist_warm_reset(void)
{
	Reset_Warm();
}

/* ---- Starting a file ------------------------------------------------------ */

static int is_floppy_image(const char *path)
{
	static const char *const exts[] = {".st", ".msa", ".dim", ".stx", ".ipf", ".scp",
	                                   ".zip", ".gz", NULL};
	const char *dot = strrchr(path, '.');
	int i;
	if (dot == NULL)
		return FALSE;
	for (i = 0; exts[i] != NULL; i++)
		if (strcasecmp(dot, exts[i]) == 0)
			return TRUE;
	return FALSE;
}

/* The web UI's "start this file": a floppy image goes in drive A and the ST
   starts again from it. */
static void autostart(const char *path)
{
	if (!is_floppy_image(path)) {
		printf("Web UI: %s is not a floppy image\n", path);
		osd_notify("Only floppy images can be started");
		return;
	}
	printf("Web UI: starting %s\n", path);
	bmatarist_settings_begin();
	if (Floppy_SetDiskFileName(0, path, NULL) == NULL) {
		osd_notify("Cannot open that floppy image");
		return;
	}
	bmatarist_settings_apply(TRUE);
}

/* ---- Requests from the web UI ------------------------------------------ */

static volatile int request_power = -1;   /* 0 reboot, 1 power off */
static volatile int request_coldstart = FALSE;
static char autostart_path[600];
static volatile int request_autostart = FALSE;

void bmatarist_request_power(int power_off)
{
	request_power = power_off ? 1 : 0;
}

/* Called by the web UI. */
void bmatarist_request_coldstart(void)
{
	request_coldstart = TRUE;
}

/* Called by the web UI ("start this file"), on core 0: the emulator takes
   it up between frames (handle_requests()). One at a time. */
void emu_autostart_interrupt(const char *path)
{
	if (request_autostart)
		return;
	snprintf(autostart_path, sizeof autostart_path, "%s", path);
	__sync_synchronize();
	request_autostart = TRUE;
}

int bmatarist_pending_requests(void)
{
	return request_power >= 0 || request_coldstart || request_autostart;
}

static void power(int off)
{
	int d;
	printf("%s\n", off ? "Shutting down" : "Rebooting");
	/* Changed floppies are written back as Hatari does when they are
	   ejected. */
	for (d = 0; d < MAX_FLOPPYDRIVES; d++)
		if (EmulationDrives[d].bContentsChanged)
			Floppy_EjectDiskFromDrive(d);
	sync_files();
	if (off)
		host_power_off();
	else
		host_reboot();
}

static void handle_requests(void)
{
	if (request_autostart) {
		__sync_synchronize();
		autostart(autostart_path);
		request_autostart = FALSE;
	}
	if (request_coldstart) {
		request_coldstart = FALSE;
		printf("Web UI: restart\n");
		bmatarist_cold_reset();
	}
	if (request_power >= 0) {
		int off = request_power;
		request_power = -1;
		power(off);
	}
}

static double last_sync_time;

void bmatarist_idle(void)
{
	double now = host_time();
	bmatarist_feed_silence();
	if (now - last_sync_time >= 1.0) {
		last_sync_time = now;
		write_back_floppies(now);
		sync_files();
	}
	if (request_power >= 0)
		handle_requests();
}

/* ---- Hatari's front end functions (main_sdl.c, dialogs) ------------------ */

static int quit_value;

void Main_RequestQuit(int exitval)
{
	/* There's nothing to quit to: the menu powers off or reboots. */
	quit_value = exitval;
}

void Main_SetQuitValue(int exitval)
{
	quit_value = exitval;
}

static char error_text[160];

/* Hatari can't go on (it couldn't load the TOS, for one): the main loop
   shows why. */
void Main_ErrorExit(const char *msg1, const char *msg2, int errval)
{
	snprintf(error_text, sizeof error_text, "%s%s%s", msg1 ? msg1 : "Hatari stopped",
	         msg2 ? ": " : "", msg2 ? msg2 : "");
	printf("Hatari: error %d: %s\n", errval, error_text);
	bQuitProgram = true;
	M68000_SetSpecial(SPCFLAG_BRK);
}

/* Hatari asks (dialog.c's reset question, overwriting a file...): BMAtariST
   doesn't, and the answer is no. Its notices go to the notification line. */
bool DlgAlert_Query(const char *text)
{
	printf("Hatari asks: %s (no)\n", text);
	return false;
}

bool DlgAlert_Notice(const char *text)
{
	printf("Hatari: %s\n", text);
	osd_notify(text);
	return true;
}

void Dialog_HaltDlg(void)
{
	/* The CPU halted (a double bus error): as on a real ST, it stays so
	   until reset. */
	osd_notify("The ST has halted: reset it from the menu (F12)");
}

/* Hatari's settings dialog (its F12, and when the TOS can't be loaded):
   BMAtariST's menu does that. */
int Dialog_MainDlg(bool *bReset, bool *bLoadedSnapshot)
{
	*bReset = false;
	*bLoadedSnapshot = false;
	return 0;
}

char *DlgFloppy_ShortCutSel(const char *path_and_name, char **zip_path)
{
	return NULL;
}

/* ---- Starting ----------------------------------------------------------- */

static void show_network_address(void)
{
	static int shown;
	char address[32];
	if (shown || !option_on("webui", TRUE))
		return;
	if (host_network_address(address, sizeof(address))) {
		char text[64];
		snprintf(text, sizeof(text), "Web UI: http://%s/", address);
		osd_notify(text);
		shown = TRUE;
	}
}

static void stopped(const char *why)
{
	char text[256];
	snprintf(text, sizeof text, "%s. The log (see the User Guide) says more.", why);
	osd_open();
	osd_message(text, FALSE);
	for (;;) {
		bmatarist_idle();
		host_video_present(TRUE);
	}
}

void bmatarist_main(void)
{
	static char *argv[16];
	static char rate_arg[16];
	static char tos_arg[256];
	struct stat st;
	int argc = 0, pitch;
	unsigned rate;
	double last_second, frame_due = 0.0;
	int vbls_at_second;
	int first_frame = TRUE;

	if (host_video_init(BMATARIST_FB_W, BMATARIST_FB_H, &fb_pixels, &pitch) != 0) {
		printf("BMAtariST: no display\n");
		for (;;)
			host_sleep_us(1000000);
	}
	fb_pitch_px = pitch / 2;
	clear_framebuffer();
	bmatarist_screen_init(fb_pixels, fb_pitch_px);
	apply_view();
	host_display_match_rate(50.0);
	measure_refresh_rate();

	/* Hatari's home folder (where it would keep its files) is the card. */
	setenv("HOME", bmatarist_sd_root(), 1);
	bmatarist_card_path(hatari_cfg, sizeof hatari_cfg, "", "hatari.cfg");

	find_tos();
	rate = bmatarist_sound_open();

	argv[argc++] = "hatari";
	argv[argc++] = "--configfile";
	argv[argc++] = hatari_cfg;
	/* Sound at the output's rate (it's fixed: 44.1 kHz on the Pi Zero to
	   3, 48 kHz over the Pi 4 and 5's HDMI). */
	if (rate != 0 && option_on("sound", TRUE)) {
		snprintf(rate_arg, sizeof rate_arg, "%u", rate);
		argv[argc++] = "--sound";
		argv[argc++] = rate_arg;
	} else {
		argv[argc++] = "--sound";
		argv[argc++] = "off";
	}
	/* A new card's first start, or a TOS image that's gone: the TOS
	   BMAtariST found. Hatari keeps it in hatari.cfg. */
	if (stat(hatari_cfg, &st) != 0) {
		/* --configfile wants the file to be there: an empty one, which
		   Hatari fills in when it saves its settings. */
		const char *tos = default_tos();
		FILE *f = fopen(hatari_cfg, "w");
		if (f != NULL)
			fclose(f);
		else
			printf("BMAtariST: cannot create %s\n", hatari_cfg);
		if (tos != NULL) {
			snprintf(tos_arg, sizeof tos_arg, "%s", tos);
			argv[argc++] = "--tos";
			argv[argc++] = tos_arg;
		}
	} else if (!cfg_tos_exists(hatari_cfg)) {
		const char *tos = default_tos();
		if (tos != NULL) {
			printf("BMAtariST: the TOS in hatari.cfg isn't there, using %s\n", tos);
			snprintf(tos_arg, sizeof tos_arg, "%s", tos);
			argv[argc++] = "--tos";
			argv[argc++] = tos_arg;
		}
	}
	argv[argc] = NULL;
	if (tos_count == 0)
		printf("BMAtariST: no TOS image in tos/ (see tos/README.txt)\n");

	Main_Init(argc, argv);
	if (bQuitProgram || !bTosImageLoaded)
		stopped(error_text[0] ? error_text : "Hatari did not start");
	/* Settings given on the command line (the TOS) are kept. */
	Configuration_Save();

	Main_UnPauseEmulation();
	match_display_rate();
	apply_view();

	last_second = host_time();
	vbls_at_second = nVBLs;
	for (;;) {
		double now;

		/* A frame: the CPU runs until the next VBL (timing.c). After a
		   reset (Hatari's UAE_RESET, or a CPU mode change), m68k_go()
		   restarts the CPU core as Hatari's other front ends do; going
		   on with m68k_run() would run at the old program counter. */
		if (first_frame) {
			first_frame = FALSE;
			UAE_Set_Quit_Reset(false);
			m68k_go(true);
		} else if (quit_program == UAE_RESET || quit_program == UAE_RESET_HARD
		           || (regs.spcflags & SPCFLAG_MODE_CHANGE)) {
			m68k_go(true);
		} else {
			M68000_UnsetSpecial(SPCFLAG_BRK);
			quit_program = 0;
			m68k_run();
		}
		if (bQuitProgram)
			stopped(error_text[0] ? error_text : "Hatari stopped");

		if (!osd_view) {
			bmatarist_screen_copy(FALSE);
			bmatarist_draw_disk_activity(fb_pixels, fb_pitch_px,
			                             BMATARIST_FB_W, BMATARIST_FB_H);
			{
				int w, h;
				double aspect;
				bmatarist_screen_view(&w, &h, &aspect);
				if (osd_draw_notification(fb_pixels, fb_pitch_px, w, h))
					bmatarist_screen_dirty_bottom();
			}
		}
		host_video_present(vsync_lock);
		if (!vsync_lock) {
			/* Paced by the clock instead. */
			double period = 1.0 / st_rate();
			now = host_time();
			if (frame_due == 0.0 || now - frame_due > 0.25)
				frame_due = now;
			frame_due += period;
			if (frame_due > now)
				host_sleep_us((unsigned) ((frame_due - now) * 1000000.0));
		}

		if (bmatarist_menu_requested()) {
			bmatarist_release_keys();
			Main_PauseEmulation(false);
			bmatarist_menu();
			Main_UnPauseEmulation();
			apply_view();
			bmatarist_screen_copy(TRUE);
			frame_due = 0.0;
			last_second = host_time();
			vbls_at_second = nVBLs;
		}

		now = host_time();
		if (now - last_second >= 1.0) {
			speed_percent = (int) ((nVBLs - vbls_at_second) / (now - last_second)
			                       / st_rate() * 100.0 + 0.5);
			last_second = now;
			vbls_at_second = nVBLs;
			match_display_rate();
			write_back_floppies(now);
			sync_files();
			show_network_address();
		}
		handle_requests();
	}
}
