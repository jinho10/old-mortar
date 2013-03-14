/******************************************************************************
 * mcd.h
 * 
 * Guest OS interface to Xen Memcached
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Copyright (c) 2004, K A Fraser
 */

#ifndef __XEN_PUBLIC_MCD_H__
#define __XEN_PUBLIC_MCD_H__

#include "xen.h"

/* version of ABI */
#define MCD_VERSION                 1

/* TODO modifying Special errno values */
#define EFROZEN                 1000
#define EEMPTY                  1001

typedef xen_pfn_t mcd_mfn_t;
typedef XEN_GUEST_HANDLE(void) mcd_va_t;

/*
 * Basic operations of Hypervisor Memcached
 */
#define XENMCD_cache_get        1
#define XENMCD_cache_put        2
#define XENMCD_cache_remove     3
#define XENMCD_cache_check      4
#define XENMCD_cache_getsize    5
#define XENMCD_cache_flush      6
#define XENMCD_stat_get         7

typedef struct mcd_arg {
#define MCDOPT_private          1
#define MCDOPT_shared           2
    uint32_t option;
    uint32_t key_size;
    uint32_t val_size;
    mcd_va_t r_val_size;
    mcd_va_t key;
    mcd_va_t val;
} mcd_arg_t;
DEFINE_XEN_GUEST_HANDLE(mcd_arg_t);

/*
 * Returns the output of statistics
 */
#define XENMCD_display_stats        100
typedef struct mcd_display_stats {
    uint32_t domid;
    uint32_t option;
    uint32_t buf_len;
    mcd_va_t buf;
} mcd_display_stats_t;
DEFINE_XEN_GUEST_HANDLE(mcd_display_stats_t);

/*
 * Controls
 */
#define XENMCD_memsize              101
typedef struct mcd_memsize {
    uint32_t memsize;
    uint32_t buf_len;
    mcd_va_t buf;
} mcd_memsize_t;
DEFINE_XEN_GUEST_HANDLE(mcd_memsize_t);

#define XENMCD_weight               102
typedef struct mcd_weight {
    uint32_t domid;
    uint32_t weight;
    uint32_t buf_len;
    mcd_va_t buf;
} mcd_weight_t;
DEFINE_XEN_GUEST_HANDLE(mcd_weight_t);

#define XENMCD_cache_mode           103
typedef struct mcd_cache_mode {
    /* 0: sharing, 1: replacement */
    uint32_t which;
    uint32_t cache_mode;
    uint32_t buf_len;
    mcd_va_t buf;
} mcd_cache_mode_t;
DEFINE_XEN_GUEST_HANDLE(mcd_cache_mode_t);

#define XENMCD_ssd_param            104
typedef struct mcd_ssd_param {
    /* 0: sharing, 1: replacement */
    uint32_t which;
    uint32_t param;
    uint32_t buf_len;
    mcd_va_t buf;
} mcd_ssd_param_t;
DEFINE_XEN_GUEST_HANDLE(mcd_ssd_param_t);

/*
 * Test for Hypercall: Print to dmesg
 */
#define XENMCD_hypercall_test       1000

#endif /* __XEN_PUBLIC_MCD_H__ */
/*
 * Local variables:
 * mode: C
 * c-set-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
