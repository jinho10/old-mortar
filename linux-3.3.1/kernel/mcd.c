#include <linux/linkage.h>
#include <linux/kernel.h>
#include <linux/unistd.h>
#include <xen/mcd.h>

/*
 * System Call __NR_mcd Wrapper
 */
asmlinkage long sys_mcd(uint32_t cmd, void __user *arg) 
{
	return xen_mcd_gateway(cmd, arg);
}
