/******************************************************************************
 * mcd.h
 *
 * Memcached
 *
 * Copyright (c) 2012, Jinho Hwang, The George Washington University
 */

#ifndef __MCD_H__
#define __MCD_H__

#include <xen/config.h>
#include <xen/mm.h> /* heap alloc/free */
#include <xen/xmalloc.h> /* xmalloc/xfree */
#include <xen/sched.h>  /* struct domain */
#include <xen/guest_access.h> /* copy_from_guest */
#include <xen/hash.h> /* hash_long */
#include <xen/domain_page.h> /* __map_domain_page */
#include <public/mcd.h>

extern bool_t use_mcd_flag;
static inline bool_t use_mcd(void)
{
    return use_mcd_flag;
}

extern bool_t mcd_enabled_flag;
static inline bool_t mcd_enabled(void)
{
    return mcd_enabled_flag;
}

// TODO
//typedef XEN_GUEST_HANDLE(mcd_op_t) mcd_cli_op_t;
//extern long do_mcd_op(mcd_cli_op_t uops);

extern void mcd_destroy(void *p);

// TODO can be removed by calling this when it happens for the first time
// but for now, just make it when domain is created in domain.c
extern int mcd_domain_create(domid_t domid);


/* 
 * Memory ballooning traps
 */ 
extern void mcd_mem_inc_trap(struct domain *dom, unsigned int nr_pages);
extern void mcd_mem_dec_trap(struct domain *dom, unsigned int nr_pages);
extern void mcd_mem_upt_trap(struct domain *dom);

#endif /* __MCD_H__ */
