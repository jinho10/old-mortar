/*
 * Xen implementation for hypervisor memcached (mcd)
 *
 * Copyright (C) 2012 The George Washington University.  All rights reserved.
 * Author: Jinho Hwang
 */

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/init.h>
#include <linux/pagemap.h>
#include <linux/module.h>
#include <linux/cleancache.h>

#include <xen/xen.h>
#include <xen/interface/xen.h>
#include <asm/xen/hypercall.h>
#include <asm/xen/page.h>
#include <asm/xen/hypervisor.h>

#include <xen/tmem.h>
#include <linux/mcd.h>
#include <xen/mcd.h>

#include <linux/vmalloc.h>

/*
 * Hypercall Functions
 */
static int xen_mcd_op(uint32_t cmd, mcd_data_t *md) 
{
	struct mcd_op op;
	int rc = 0;

    op.option = md->option;
    op.key_size = md->key_size;
    op.val_size = md->val_size;

    set_xen_guest_handle(op.r_val_size, (void *)md->r_val_size);
    set_xen_guest_handle(op.key, (void *)md->key);
    set_xen_guest_handle(op.val, (void *)md->val);

	rc = HYPERVISOR_mcd_op(cmd, &op);

	return rc;
}

int __xen_mcd_gateway(uint32_t cmd, mcd_data_t *md)
{
    unsigned char key[md->key_size];
    unsigned char *val = NULL; // prevent stack overflow
    int r_val_size;

    mcd_data_t imd;
    int rc = 0;

    /*
     * Prior Work
     */
    switch ( cmd ) {
    case XENMCD_cache_put:
        val = (char*) vmalloc(md->val_size);
        if ( !val ) goto error_ret;
        rc = copy_from_user(val, md->val, md->val_size);
        if ( rc < 0 ) return rc;

        break;
    case XENMCD_cache_get:
        //in this case, user defined val size
        //val = (char*) kmalloc(md->val_size, GFP_KERNEL); // not used
        val = (char*) vmalloc(md->val_size);
        if ( !val ) goto error_ret;
        break;
    case XENMCD_cache_remove:
    case XENMCD_cache_check:
    case XENMCD_cache_getsize:
    case XENMCD_cache_flush:
        break;
    default:
        break;
    }

    // Default Value (Could be 0)
    if ( md->key_size != 0 && md->key != NULL ) {
        rc = copy_from_user(key, md->key, md->key_size);
        if ( rc < 0 ) goto error_ret;
    }

    // Keep User-Space Pointers
    imd.option = md->option;
    imd.key_size = md->key_size;
    imd.val_size = md->val_size;
    imd.key = key;
    imd.val = val;
    imd.r_val_size = &r_val_size;

	rc = xen_mcd_op(cmd, &imd);
    if ( rc < 0 ) goto error_ret;

    /*
     * Posterior Work
     */
    switch ( cmd ) {
    case XENMCD_cache_get:
        rc = copy_to_user(md->val, val, *imd.r_val_size);
        if ( rc < 0 ) goto error_ret;
    case XENMCD_cache_put:
        //kfree(val);
        vfree(val);
    case XENMCD_cache_check:
    case XENMCD_cache_remove:
    case XENMCD_cache_getsize:
    case XENMCD_cache_flush:
        rc = copy_to_user(md->r_val_size, imd.r_val_size, sizeof(int));
        if ( rc < 0 ) goto error_ret;
        break;
    default:
        break;
    }

    return rc;

error_ret:
    if ( !val )
        vfree(val);

    copy_to_user(md->r_val_size, &rc, sizeof(int));
    return rc;
}
EXPORT_SYMBOL(__xen_mcd_gateway);

/*
 * Initialization Codes
 */
int mcd_use __read_mostly;
int mcd_enabled __read_mostly;
EXPORT_SYMBOL(mcd_enabled);
                   
static int __init use_mcd(char *s)
{
	mcd_use = 1;
	return 1;
}
__setup("mcd", use_mcd);

static int __init xen_mcd_init(void)
{
	if ( !xen_domain() )
		return 0;

	if ( xen_initial_domain() ) {
		printk(KERN_INFO "xen/mcd: xen memcached driver "
					"disabled for dom0.\n");
		return -ENODEV;
	} 

	if ( tmem_enabled ) {
		printk(KERN_INFO "memcached (mcd) enabled ");
		mcd_enabled = 1;
	}

	// TODO : any initialization

	return 0;
}

module_init(xen_mcd_init)
