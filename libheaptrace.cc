/* Copyright (c) 2022 LG Electronics Inc. */
/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>
#include <dlfcn.h>
#include <signal.h>

#include "heaptrace.h"
#include "compiler.h"
#include "stacktrace.h"
#include "sighandler.h"

// dlsym internally uses calloc, so use weak symbol to get their symbol
extern "C" __weak void* __libc_malloc(size_t size);
extern "C" __weak void  __libc_free(void* ptr);
extern "C" __weak void* __libc_calloc(size_t nmemb, size_t size);
extern "C" __weak void* __libc_realloc(void *ptr, size_t size);
extern "C" __weak int   __posix_memalign(void **memptr, size_t alignment, size_t size);

typedef void* (*MallocFunction)(size_t size);
typedef void  (*FreeFunction)(void *ptr);
typedef void* (*CallocFunction)(size_t nmemb, size_t size);
typedef void* (*ReallocFunction)(void *ptr, size_t size);
typedef int   (*PosixMemalignFunction)(void **memptr, size_t alignment, size_t size);

static MallocFunction        real_malloc;
static FreeFunction          real_free;
static CallocFunction        real_calloc;
static ReallocFunction       real_realloc;
static PosixMemalignFunction real_posix_memalign;

thread_local struct thread_flags_t thread_flags;

struct opts opts;

__constructor
static void heaptrace_init()
{
	auto* tfs = &thread_flags;
	struct sigaction sigusr1, sigusr2;
	int pid = getpid();

	real_malloc = (MallocFunction)dlsym(RTLD_NEXT, "malloc");
	real_free = (FreeFunction)dlsym(RTLD_NEXT, "free");
	real_calloc = (CallocFunction)dlsym(RTLD_NEXT, "calloc");
	real_realloc = (ReallocFunction)dlsym(RTLD_NEXT, "realloc");
	real_posix_memalign = (PosixMemalignFunction)dlsym(RTLD_NEXT, "posix_memalign");

	sigusr1.sa_handler = sigusr1_handler;
	sigemptyset(&sigusr1.sa_mask);
	sigusr1.sa_flags = 0;

	sigusr2.sa_handler = sigusr2_handler;
	sigemptyset(&sigusr2.sa_mask);
	sigusr2.sa_flags = 0;

	if (sigaction(SIGUSR1, &sigusr1, 0) == -1)
		pr_dbg("signal(SIGUSR1) error");

	if (sigaction(SIGUSR2, &sigusr2, 0) == -1)
		pr_dbg("signal(SIGUSR2) error");

	// setup option values
	opts.top = strtol(getenv("HEAPTRACE_NUM_TOP_BACKTRACE"), NULL, 0);

	pr_out("[heaptrace] initialized for /proc/%d/maps\n", pid);

	tfs->initialized = true;
}

__destructor
static void heaptrace_fini()
{
	auto* tfs = &thread_flags;
	int pid = getpid();

	pr_out("[heaptrace] finalized for /proc/%d/maps\n", pid);
	dump_stackmap(ALLOC_SIZE);

	// disable any other hooking after this.
	tfs->hook_guard = true;
}

extern "C"
void* malloc(size_t size)
{
	auto* tfs = &thread_flags;

	if (unlikely(tfs->hook_guard || !tfs->initialized))
		return __libc_malloc(size);

	tfs->hook_guard = true;

	void* p = real_malloc(size);
	pr_dbg("malloc(%zd) = %p\n", size, p);
	record_backtrace(size, p);

	tfs->hook_guard = false;

	return p;
}

extern "C"
void free(void *ptr)
{
	auto* tfs = &thread_flags;

	if (unlikely(tfs->hook_guard || !tfs->initialized)) {
		__libc_free(ptr);
		return;
	}

	tfs->hook_guard = true;

	pr_dbg("free(%p)\n", ptr);
	release_backtrace(ptr);
	real_free(ptr);

	tfs->hook_guard = false;
}

extern "C"
void *calloc(size_t nmemb, size_t size)
{
	auto* tfs = &thread_flags;

	if (unlikely(tfs->hook_guard || !tfs->initialized))
		return __libc_calloc(nmemb, size);

	tfs->hook_guard = true;

	void* p = real_calloc(nmemb, size);
	pr_dbg("calloc(%zd, %zd) = %p\n", nmemb, size, p);
	record_backtrace(nmemb * size, p);

	tfs->hook_guard = false;

	return p;
}

extern "C"
void *realloc(void *ptr, size_t size)
{
	auto* tfs = &thread_flags;

	if (unlikely(tfs->hook_guard || !tfs->initialized))
		return __libc_realloc(ptr, size);

	tfs->hook_guard = true;

	void* p = real_realloc(ptr, size);
	pr_dbg("realloc(%p, %zd) = %p\n", ptr, size, p);
	release_backtrace(ptr);
	record_backtrace(size, p);

	tfs->hook_guard = false;

	return p;
}

extern "C"
int posix_memalign(void **memptr, size_t alignment, size_t size)
{
	auto *tfs = &thread_flags;

	if (unlikely(tfs->hook_guard || !tfs->initialized))
		return __posix_memalign(memptr, alignment, size);

	tfs->hook_guard = true;

	int ret = real_posix_memalign(memptr, alignment, size);
	pr_dbg("posix_memalign(%p, %zd, %zd) = %d\n", memptr, alignment, size, ret);
	if (ret == 0)
		record_backtrace(size, *memptr);

	tfs->hook_guard = false;

	return ret;
}
