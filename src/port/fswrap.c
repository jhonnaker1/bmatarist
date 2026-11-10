/*
 * fswrap.c - file system fixes for KEGS on Circle's FatFs.
 *
 * FatFs cannot stat() the root directory of a volume, so KEGS's file
 * browser would think "/" doesn't exist and refuse to go up to it. The
 * kernel is linked with --wrap=stat, so every call to stat() comes here
 * first; the root (and ".") are reported as directories, everything else
 * goes to the real stat().
 *
 * There is no /tmp on the card either, so newlib's tmpfile() fails, and
 * Hatari needs one to save its settings (cfgopts.c's update_config() writes
 * hatari.cfg through a temporary file; inffile.c makes the auto-start .INF
 * with one). Without it hatari.cfg stayed empty: no setting was ever saved,
 * and the next start had no TOS. --wrap=tmpfile gives a temporary file in
 * memory instead (newlib's fmemopen(), whose buffer is freed by fclose()).
 *
 * This file is part of BMAtariST, a bare metal Atari ST emulator for
 * the Raspberry Pi. See LICENSE for licensing details.
 */

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

int __real_stat(const char *path, struct stat *st);

static int is_root(const char *path)
{
	const char *colon = strchr(path, ':');
	/* Skip a volume name such as "SD:" */
	if (colon != NULL)
		path = colon + 1;
	while (*path == '/')
		path++;
	return *path == '\0' || strcmp(path, ".") == 0 || strcmp(path, "./") == 0;
}

int __wrap_stat(const char *path, struct stat *st)
{
	if (path != NULL && st != NULL && is_root(path)) {
		memset(st, 0, sizeof(*st));
		st->st_mode = S_IFDIR | S_IRWXU | S_IRWXG | S_IRWXO;
		st->st_nlink = 1;
		return 0;
	}
	return __real_stat(path, st);
}

/* hatari.cfg is about 10K, an .INF file a few K. */
#define TEMP_FILE_BYTES (256 * 1024)

FILE *__wrap_tmpfile(void)
{
	return fmemopen(NULL, TEMP_FILE_BYTES, "w+b");
}
