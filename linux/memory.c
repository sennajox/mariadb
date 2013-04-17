/* -*- mode: C; c-basic-offset: 4 -*- */
#ident "Copyright (c) 2007-2011 Tokutek Inc.  All rights reserved."

#include <toku_portability.h>
#include "db.h"   // get Toku-specific version of db.h

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <dlfcn.h>
#include "memory.h"
#include "toku_assert.h"

static malloc_fun_t  t_malloc  = 0;
static malloc_fun_t  t_xmalloc = 0;
static free_fun_t    t_free    = 0;
static realloc_fun_t t_realloc = 0;
static realloc_fun_t t_xrealloc = 0;

///////////////////////////////////////////////////////////////////////////////////
// Engine status
//
// Status is intended for display to humans to help understand system behavior.
// It does not need to be perfectly thread-safe.

static MEMORY_STATUS_S memory_status;
static volatile uint64_t max_in_use;      // maximum memory footprint (used - freed), approximate (not worth threadsafety overhead for exact, but worth keeping as volatile)

#define STATUS_INIT(k,t,l) { \
	memory_status.status[k].keyname = #k; \
	memory_status.status[k].type    = t;  \
	memory_status.status[k].legend  = "memory: " l; \
    }

static void
status_init(void) {
    // Note, this function initializes the keyname, type, and legend fields.
    // Value fields are initialized to zero by compiler.
    STATUS_INIT(MEMORY_MALLOC_COUNT,       UINT64,  "number of malloc operations");
    STATUS_INIT(MEMORY_FREE_COUNT,         UINT64,  "number of free operations");
    STATUS_INIT(MEMORY_REALLOC_COUNT,      UINT64,  "number of realloc operations");
    STATUS_INIT(MEMORY_MALLOC_FAIL,        UINT64,  "number of malloc operations that failed");
    STATUS_INIT(MEMORY_REALLOC_FAIL,       UINT64,  "number of realloc operations that failed" );
    STATUS_INIT(MEMORY_REQUESTED,          UINT64,  "number of bytes requested");
    STATUS_INIT(MEMORY_USED,               UINT64,  "number of bytes used (requested + overhead)");
    STATUS_INIT(MEMORY_FREED,              UINT64,  "number of bytes freed");
    STATUS_INIT(MEMORY_MAX_IN_USE,         UINT64,  "estimated maximum memory footprint");
    STATUS_INIT(MEMORY_MALLOCATOR_VERSION, CHARSTR, "mallocator version");
    STATUS_INIT(MEMORY_MMAP_THRESHOLD,     UINT64,  "mmap threshold");
    memory_status.initialized = 1;  // TODO 2949 Make this a bool, set to true
}
#undef STATUS_INIT

#define STATUS_VALUE(x) memory_status.status[x].value.num

void
toku_memory_get_status(MEMORY_STATUS statp) {
    if (!memory_status.initialized)
	status_init();
    STATUS_VALUE(MEMORY_MAX_IN_USE) = max_in_use;
    *statp = memory_status;
}

#define STATUS_VERSION_STRING memory_status.status[MEMORY_MALLOCATOR_VERSION].value.str

int
toku_memory_startup(void) {
    int result = 0;
   
    // initialize libc malloc
    size_t mmap_threshold = 64 * 1024; // 64K and larger should be malloced with mmap().
    int success = mallopt(M_MMAP_THRESHOLD, mmap_threshold);
    if (success) {
        STATUS_VERSION_STRING = "libc";
        STATUS_VALUE(MEMORY_MMAP_THRESHOLD) = mmap_threshold;
    } else
        result = EINVAL;

    // jemalloc has a mallctl function, while libc malloc does not.  we can check if jemalloc 
    // is loaded by checking if the mallctl function can be found.  if it can, we call it 
    // to get version and mmap threshold configuration.
    typedef int (*mallctl_fun_t)(const char *, void *, size_t *, void *, size_t);
    mallctl_fun_t mallctl_f;
    mallctl_f = (mallctl_fun_t) dlsym(RTLD_DEFAULT, "mallctl");
    if (mallctl_f) { // jemalloc is loaded
        size_t version_length = sizeof STATUS_VERSION_STRING;
        result = mallctl_f("version", &STATUS_VERSION_STRING, &version_length, NULL, 0);
        if (result == 0) {
            size_t lg_chunk; // log2 of the mmap threshold
            size_t lg_chunk_length = sizeof lg_chunk;
            result  = mallctl_f("opt.lg_chunk", &lg_chunk, &lg_chunk_length, NULL, 0);
            if (result == 0)
                STATUS_VALUE(MEMORY_MMAP_THRESHOLD) = 1 << lg_chunk;
        }
    }

    return result;
}

void
toku_memory_shutdown(void) {
}

// jemalloc's malloc_usable_size does not work with a NULL pointer, so we implement a version that works
static size_t
my_malloc_usable_size(void *p) {
    return p == NULL ? 0 : malloc_usable_size(p);
}

// Note that max_in_use may be slightly off because use of max_in_use is not thread-safe.
// It is not worth the overhead to make it completely accurate, but
// this logic is intended to guarantee that it increases monotonically.
// Note that status.sum_used and status.sum_freed increase monotonically
// and that max_in_use is declared volatile.
static inline void 
set_max(uint64_t sum_used, uint64_t sum_freed) {
    if (sum_used >= sum_freed) {
	uint64_t in_use = sum_used - sum_freed;
	uint64_t old_max;
	do {
	    old_max = max_in_use;
	} while (old_max < in_use &&
		 !__sync_bool_compare_and_swap(&max_in_use, old_max, in_use));
    }
}

size_t 
toku_memory_footprint(void * p, size_t touched) {
    static size_t pagesize = 0;
    size_t rval = 0;
    if (!pagesize)
	pagesize = sysconf(_SC_PAGESIZE);
    if (p) {
	size_t usable = my_malloc_usable_size(p);
	if (usable >= STATUS_VALUE(MEMORY_MMAP_THRESHOLD)) {
            int num_pages = (touched + pagesize) / pagesize;
            rval = num_pages * pagesize;
	}
	else {
	    rval = usable;
	}
    }
    return rval;
}

void *
toku_malloc(size_t size) {
    void *p = t_malloc ? t_malloc(size) : os_malloc(size);
    if (p) {
        size_t used = my_malloc_usable_size(p);
        __sync_add_and_fetch(&STATUS_VALUE(MEMORY_MALLOC_COUNT), 1);
        __sync_add_and_fetch(&STATUS_VALUE(MEMORY_REQUESTED), size);
        __sync_add_and_fetch(&STATUS_VALUE(MEMORY_USED), used);
        set_max(STATUS_VALUE(MEMORY_USED), STATUS_VALUE(MEMORY_FREED));
    } else {
        __sync_add_and_fetch(&STATUS_VALUE(MEMORY_MALLOC_FAIL), 1);
    }
    return p;
}

void *
toku_calloc(size_t nmemb, size_t size) {
    size_t newsize = nmemb * size;
    void *p = toku_malloc(newsize);
    if (p) memset(p, 0, newsize);
    return p;
}

void *
toku_realloc(void *p, size_t size) {
    size_t used_orig = p ? my_malloc_usable_size(p) : 0;
    void *q = t_realloc ? t_realloc(p, size) : os_realloc(p, size);
    if (q) {
	size_t used = my_malloc_usable_size(q);
	__sync_add_and_fetch(&STATUS_VALUE(MEMORY_REALLOC_COUNT), 1);
	__sync_add_and_fetch(&STATUS_VALUE(MEMORY_REQUESTED), size);
	__sync_add_and_fetch(&STATUS_VALUE(MEMORY_USED), used);
	__sync_add_and_fetch(&STATUS_VALUE(MEMORY_FREED), used_orig);
	set_max(STATUS_VALUE(MEMORY_USED), STATUS_VALUE(MEMORY_FREED));
    } else {
	__sync_add_and_fetch(&STATUS_VALUE(MEMORY_REALLOC_FAIL), 1);
    }
    return q;
}

void *
toku_memdup(const void *v, size_t len) {
    void *p = toku_malloc(len);
    if (p) memcpy(p, v,len);
    return p;
}

char *
toku_strdup(const char *s) {
    return toku_memdup(s, strlen(s)+1);
}

void
toku_free(void *p) {
    if (p) {
	size_t used = my_malloc_usable_size(p);
	__sync_add_and_fetch(&STATUS_VALUE(MEMORY_FREE_COUNT), 1);
	__sync_add_and_fetch(&STATUS_VALUE(MEMORY_FREED), used);
	if (t_free)
	    t_free(p);
	else
	    os_free(p);
    }
}

void
toku_free_n(void* p, size_t size __attribute__((unused))) {
    toku_free(p);
}

void *
toku_xmalloc(size_t size) {
    void *p = t_xmalloc ? t_xmalloc(size) : os_malloc(size);
    if (p == NULL)  // avoid function call in common case
        resource_assert(p);
    size_t used = my_malloc_usable_size(p);
    __sync_add_and_fetch(&STATUS_VALUE(MEMORY_MALLOC_COUNT), 1);
    __sync_add_and_fetch(&STATUS_VALUE(MEMORY_REQUESTED), size);
    __sync_add_and_fetch(&STATUS_VALUE(MEMORY_USED), used);
    set_max(STATUS_VALUE(MEMORY_USED), STATUS_VALUE(MEMORY_FREED));
    return p;
}

void *
toku_xcalloc(size_t nmemb, size_t size) {
    size_t newsize = nmemb * size;
    void *vp = toku_xmalloc(newsize);
    if (vp) memset(vp, 0, newsize);
    return vp;
}

void *
toku_xrealloc(void *v, size_t size) {
    size_t used_orig = v ? my_malloc_usable_size(v) : 0;
    void *p = t_xrealloc ? t_xrealloc(v, size) : os_realloc(v, size);
    if (p == 0)  // avoid function call in common case
        resource_assert(p);
    size_t used = my_malloc_usable_size(p);
    __sync_add_and_fetch(&STATUS_VALUE(MEMORY_REALLOC_COUNT), 1);
    __sync_add_and_fetch(&STATUS_VALUE(MEMORY_REQUESTED), size);
    __sync_add_and_fetch(&STATUS_VALUE(MEMORY_USED), used);
    __sync_add_and_fetch(&STATUS_VALUE(MEMORY_FREED), used_orig);
    set_max(STATUS_VALUE(MEMORY_USED), STATUS_VALUE(MEMORY_FREED));
    return p;
}

size_t 
toku_malloc_usable_size(void *p) {
    return my_malloc_usable_size(p);
}

void *
toku_xmemdup (const void *v, size_t len) {
    void *p = toku_xmalloc(len);
    memcpy(p, v, len);
    return p;
}

char *
toku_xstrdup (const char *s) {
    return toku_xmemdup(s, strlen(s)+1);
}

void
toku_set_func_malloc(malloc_fun_t f) {
    t_malloc = f;
    t_xmalloc = f;
}

void
toku_set_func_xmalloc_only(malloc_fun_t f) {
    t_xmalloc = f;
}

void
toku_set_func_malloc_only(malloc_fun_t f) {
    t_malloc = f;
}

void
toku_set_func_realloc(realloc_fun_t f) {
    t_realloc = f;
    t_xrealloc = f;
}

void
toku_set_func_xrealloc_only(realloc_fun_t f) {
    t_xrealloc = f;
}

void
toku_set_func_realloc_only(realloc_fun_t f) {
    t_realloc = f;

}

void
toku_set_func_free(free_fun_t f) {
    t_free = f;
}

#include <valgrind/drd.h>
void __attribute__((constructor)) toku_memory_drd_ignore(void);
void
toku_memory_drd_ignore(void) {
    DRD_IGNORE_VAR(memory_status);
}
