/*
 * bigalloc.c - big blocks of memory, used again once freed.
 *
 * Circle's heap keeps freed blocks in lists of a few sizes, the largest
 * 512K (HEAP_BLOCK_BUCKET_SIZES in Circle's sysconfig.h); a bigger block
 * is lost when freed. Hatari frees and allocates the Atari's memory on
 * every cold reset (TOS_InitImage() calls memory_uninit() and
 * memory_init(): ST RAM 4, 8 or 16 MB, 2 MB of ROM and I/O space, the TOS
 * file and up to 256 MB of TT-RAM), and the menu cold-resets after each
 * change of machine, TOS or memory. A floppy image's buffer is also freed
 * at every eject. So after enough of them there was no memory left.
 *
 * So the kernel is linked with --wrap=malloc,free,calloc,realloc (the
 * Makefile): the big blocks given out are noted here, and when one is
 * freed it is kept, to be given again for a block that fits in it. Every
 * other block goes to Circle as before. (Circle's calloc() and realloc()
 * don't go through malloc(), so they are wrapped too; bigalloc_size.cpp reads a block's size.)
 *
 * From BMNeXT's src/port/bigalloc.c. Only the Pi needs this: the PC test
 * harness's C library reuses memory. tools/bigalloc tests it on the PC
 * against a heap that loses big blocks as Circle's does.
 *
 * This file is part of BMAtariST, a bare metal Atari ST emulator for the
 * Raspberry Pi. See LICENSE for licensing details.
 */

#ifndef BMATARIST_HOST_LIBC

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Blocks bigger than Circle's largest bucket. */
#define BIG_BYTES 0x80000
#define MAX_BLOCKS 64

void *__real_malloc(size_t size);
void __real_free(void *block);
void *__real_realloc(void *block, size_t size);
void *__wrap_malloc(size_t size);
void __wrap_free(void *block);
void *__wrap_calloc(size_t count, size_t size);
void *__wrap_realloc(void *block, size_t size);
size_t bigalloc_block_size(const void *block); /* bigalloc_size.cpp */

/* Entries are only ever added (under LOCK); free() finds its block
   without the lock, as it may come from anywhere (another core, an
   interrupt). An entry's IN_USE is claimed atomically. */
static struct {
	void *block;
	size_t size;
	int in_use;
} blocks[MAX_BLOCKS];
static int count;
static volatile int lock;

/* The entry of a big block given out, or -1. */
static int find(const void *block)
{
	int i, n = __atomic_load_n(&count, __ATOMIC_ACQUIRE);
	for (i = 0; i < n; i++)
		if (blocks[i].block == block)
			return i;
	return -1;
}

/* Notes a big block Circle just gave out (it is in use). */
static void note(void *block, size_t size)
{
	int n;
	while (__atomic_exchange_n(&lock, 1, __ATOMIC_ACQUIRE))
		;
	n = count;
	if (n < MAX_BLOCKS) {
		blocks[n].block = block;
		blocks[n].size = size;
		blocks[n].in_use = 1;
		__atomic_store_n(&count, n + 1, __ATOMIC_RELEASE);
	}
	__atomic_store_n(&lock, 0, __ATOMIC_RELEASE);
}

/* The smallest kept block SIZE fits in, now in use, or NULL. */
static void *reuse(size_t size)
{
	int i, n, best;
	for (;;) {
		n = __atomic_load_n(&count, __ATOMIC_ACQUIRE);
		best = -1;
		for (i = 0; i < n; i++)
			if (!__atomic_load_n(&blocks[i].in_use, __ATOMIC_ACQUIRE)
			    && blocks[i].size >= size
			    && (best < 0 || blocks[i].size < blocks[best].size))
				best = i;
		if (best < 0)
			return NULL;
		int free_entry = 0;
		if (__atomic_compare_exchange_n(&blocks[best].in_use, &free_entry, 1, 0,
		                                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
			return blocks[best].block;
	}
}

void *__wrap_malloc(size_t size)
{
	void *block;
	if (size <= BIG_BYTES)
		return __real_malloc(size);
	block = reuse(size);
	if (block != NULL)
		return block;
	block = __real_malloc(size);
	if (block != NULL)
		note(block, size);
	return block;
}

void __wrap_free(void *block)
{
	int i;
	if (block == NULL)
		return;
	i = find(block);
	if (i >= 0) {
		__atomic_store_n(&blocks[i].in_use, 0, __ATOMIC_RELEASE);
		return;
	}
	__real_free(block);
}

void *__wrap_calloc(size_t count_, size_t size)
{
	void *block;
	size_t bytes = count_ * size;
	if (size != 0 && bytes / size != count_)
		return NULL;
	if (bytes == 0)
		bytes = 1;
	block = __wrap_malloc(bytes);
	if (block != NULL)
		memset(block, 0, bytes);
	return block;
}

void *__wrap_realloc(void *block, size_t size)
{
	void *new_block;
	size_t old_size;
	int i;
	if (block == NULL)
		return __wrap_malloc(size);
	if (size == 0) {
		__wrap_free(block);
		return NULL;
	}
	i = find(block);
	if (i >= 0) {
		/* A kept block: it stays here, however small it becomes. */
		if (size <= blocks[i].size)
			return block;
		new_block = __wrap_malloc(size);
		if (new_block == NULL)
			return NULL;
		memcpy(new_block, block, blocks[i].size);
		__wrap_free(block);
		return new_block;
	}
	if (size <= BIG_BYTES)
		return __real_realloc(block, size);
	/* A block from Circle grows big: it moves to a big block (a kept one
	   if one fits), as Circle's realloc() would, which keeps nothing. */
	old_size = bigalloc_block_size(block);
	if (old_size >= size)
		return block;
	new_block = __wrap_malloc(size);
	if (new_block == NULL)
		return NULL;
	memcpy(new_block, block, old_size);
	__real_free(block);
	return new_block;
}

#endif
