#ifndef __XEN_MCD_H__
#define __XEN_MCD_H__

#include <asm/uaccess.h>
#include <linux/mcd.h>

/*
 * Bridge between syscall and hypercall
 * User space to Kernel space
 */
extern int __xen_mcd_gateway(uint32_t cmd, mcd_data_t *md);
extern int mcd_enabled;  

static inline int xen_mcd_gateway(uint32_t cmd, void __user *arg)
{                        
    mcd_data_t kmd;
    int rc = 0;

    if ( !mcd_enabled ) return -ERR_NOMCD;

    rc = copy_from_user(&kmd, arg, sizeof(mcd_data_t));
    if ( rc < 0 ) return rc;

    rc = __xen_mcd_gateway(cmd, &kmd);

    return rc;
}      

#endif /* __XEN_MCD_H__ */
