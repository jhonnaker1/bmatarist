/*
 * test_bigalloc.c - tests src/port/bigalloc.c on the PC.
 *
 * The __real_ functions here behave like Circle's heap: a freed block
 * bigger than 512K is lost (counted in LOST and never given again), and
 * realloc() gives back the same block when the new size fits. The test
 * repeats Hatari's cold reset (memory_uninit() and memory_init() in
 * cpu/memory.c, plus the TOS file), floppy changes and growing realloc()
 * buffers many times, and checks that the memory taken from the heap stays
 * what the first round took.
 *
 * Run: ./run_tests.sh
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BIG 0x80000

void *__wrap_malloc(size_t size);
void __wrap_free(void *block);
void *__wrap_calloc(size_t count, size_t size);
void *__wrap_realloc(void *block, size_t size);

static size_t taken, lost;

typedef struct { size_t size; size_t pad; } header;

void *__real_malloc(size_t size)
{
	header *h = malloc(sizeof *h + size);
	h->size = size;
	taken += size;
	memset(h + 1, 0xa5, size); /* not zero, so calloc must clear */
	return h + 1;
}

void __real_free(void *block)
{
	header *h = (header *)block - 1;
	if (block == NULL)
		return;
	if (h->size > BIG)
		lost += h->size; /* Circle never reuses it (keep it allocated) */
	else {
		taken -= h->size;
		free(h);
	}
}

size_t bigalloc_block_size(const void *block)
{
	return ((const header *)block - 1)->size;
}

void *__real_realloc(void *block, size_t size)
{
	header *h = (header *)block - 1;
	void *n;
	if (h->size >= size)
		return block;
	n = __real_malloc(size);
	memcpy(n, block, h->size);
	__real_free(block);
	return n;
}

/* One cold reset of a Falcon/TT with 14 MB ST RAM and 256 MB TT-RAM. */
static void cold_reset(void **st, void **rom, void **tt, int round)
{
	/* TOS_InitImage(): the TOS file is loaded, then memory_uninit(),
	   memory_init(), and the file is freed. */
	void *tos = __wrap_malloc(1024 * 1024); /* EmuTOS 1024k */
	__wrap_free(*tt);
	__wrap_free(*st);
	__wrap_free(*rom);
	*st = __wrap_malloc(0x1000000 + 640 * 400 / 2); /* sizes vary a little */
	*rom = __wrap_malloc(2 * 1024 * 1024);
	*tt = __wrap_malloc(256u * 1024 * 1024);
	assert(*st && *rom && *tt && tos);
	memset(*st, round, 4096);
	__wrap_free(tos);
}

int main(void)
{
	void *st = NULL, *rom = NULL, *tt = NULL, *disk, *buf;
	size_t after_first;
	int i;

	/* Cold resets with floppy changes (720K and 1.44 MB images) between:
	   after the first few rounds the heap must not grow any more. */
	for (i = 0; i < 1000; i++) {
		if (i == 10)
			after_first = taken + lost;
		cold_reset(&st, &rom, &tt, i);
		disk = __wrap_malloc(i & 1 ? 737280 : 1474560);
		__wrap_free(disk);
	}
	assert(taken + lost == after_first);
	assert(taken + lost < 300u * 1024 * 1024);

	/* calloc() clears a reused block. */
	buf = __wrap_calloc(1, 2 * 1024 * 1024);
	for (i = 0; i < 2 * 1024 * 1024; i++)
		assert(((unsigned char *)buf)[i] == 0);
	memset(buf, 7, 2 * 1024 * 1024);
	__wrap_free(buf);
	buf = __wrap_calloc(2 * 1024, 1024);
	assert(((unsigned char *)buf)[12345] == 0);
	__wrap_free(buf);

	/* realloc() growing a small block into a big one (hdc.c's buffer),
	   then bigger, keeps its contents and loses nothing once freed. */
	size_t before = taken + lost;
	for (i = 0; i < 100; i++) {
		buf = __wrap_realloc(NULL, 1000);
		memset(buf, 0x3c, 1000);
		buf = __wrap_realloc(buf, 600000);
		assert(((unsigned char *)buf)[999] == 0x3c);
		memset(buf, 0x5a, 600000);
		buf = __wrap_realloc(buf, 3 * 1024 * 1024);
		assert(((unsigned char *)buf)[599999] == 0x5a);
		buf = __wrap_realloc(buf, 100); /* shrinking keeps it */
		__wrap_free(buf);
	}
	assert(taken + lost <= before + 600000 + 3 * 1024 * 1024);
	assert(__wrap_realloc(__wrap_malloc(10), 0) == NULL);

	/* Small blocks go straight to the heap and back. */
	size_t small = taken;
	buf = __wrap_malloc(1000);
	__wrap_free(buf);
	assert(taken == small);

	printf("bigalloc: ok (heap used %zu MB, lost %zu MB after 1000 resets)\n",
	       (taken + lost) >> 20, lost >> 20);
	return 0;
}
