#ifndef __LINUX_MCD_H__
#define __LINUX_MCD_H__

/*
 * Commands list, sync with Xen Part
 */
#define XENMCD_cache_get        1
#define XENMCD_cache_put        2
#define XENMCD_cache_remove     3
#define XENMCD_cache_check      4
#define XENMCD_cache_getsize    5
#define XENMCD_cache_flush      6

/*
 * MCD Data Structure
 */
typedef struct mcd_data {
#define MCDOPT_private      1
#define MCDOPT_shared       2
    unsigned int option;
 
    unsigned int  key_size;     /* Key size */
    unsigned int  val_size;     /* Value size, when get, buffer size */
    int           *r_val_size;  /* Return value from hypervisor */
    unsigned char *key;         /* Key buffer */
    unsigned char *val;         /* Value buffer, when get, needs to be exact length */
} mcd_data_t;

/*
 * Errors
 */
#define ERR_NOMCD 1000 
#define ERR_PARAM 1001 

/*
 * User Interfaces
 */
#ifndef __KERNEL__

#include <linux/unistd.h>
 
/*
 * Hypervisor Memcached Entry Functions
 */
static long mcd(unsigned int cmd, mcd_data_t *md) 
{
    return ( md != NULL ) ? syscall(__NR_mcd, cmd, md) : -ERR_PARAM; 
}
 
static long mcd_verbose(unsigned int cmd, unsigned int opt, 
                 	unsigned int key_size, char *key, unsigned int val_size,
                 	int *r_val_size, char *val)
{
    // TODO sanity check for crazy values
    mcd_data_t md;
 
    md.option = opt; 
    md.key_size = key_size;
    md.key = key; 
    md.val_size = val_size;
    md.r_val_size = r_val_size;
    md.val = val; 
 
    return mcd(cmd, &md);
}
 
#endif /* __KERNEL__ */

#endif /* __LINUX_MCD_H__ */
