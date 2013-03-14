/******************************************************************************
 * mcd.c
 *
 * Memcached
 *
 * Copyright (c) 2012, Jinho Hwang, The George Washington University
 */

#if 0
#ifdef __XEN__
#include <xen/tmem_xen.h> /* host-specific (eg Xen) code goes here */
#endif
#endif

#include <xen/mcd.h>
#include <xen/tmem.h>
#include <xen/rbtree.h>
#include <xen/radix-tree.h>
#include <xen/list.h>
#include <xen/time.h>
//#include <xen/spinlock.h>

// TODO TO header

/*
 * Domain Management
 */
typedef struct mcd_hashT {
    struct list_head hash_list;

    spinlock_t lock;

    uint32_t nr_pages;
    uint32_t nr_bytes;
    uint32_t ref;
    s_time_t put_time;

    uint32_t hash;
    uint32_t key_size;
    uint32_t val_size;
    char*    key_val;
} mcd_hashT_t;

typedef struct mcd_domain {
    struct list_head mcd_list;
    struct list_head mcd_hash_list_head;

    spinlock_t      lock;

#define mcd_domid(m)    (m)->domid
    domid_t         domid;
#define domptr(m)   (m)->domain
    struct domain   *domain;

    unsigned int    nr_avail; // can be used, but we need policy
    unsigned int    nr_used_pages;
    uint64_t        nr_used_bytes;

#define DOM_WEIGHT_DEFAULT  100
    unsigned int    weight; // default 100
    unsigned int    ratio; // 0 - 100 in total
    unsigned int    curr; // 0 - (over 100 is ok)
} mcd_domain_t;

unsigned int total_weight = 0;

// SSD-related User Control
unsigned int ssd_r_latency_us = 50;
unsigned int ssd_w_latency_us = 50;
unsigned int ssd_usage_rate_percent = 100; // same size as cache

#define mcd_dom_is_dying(p) ((p)->domain->is_dying)
#define domain_is_dying(p) ((p)->is_dying)

#define MCD_DOM_ID_NULL ((domid_t)((domid_t)-1L))

/*
 * Need to Synchronize with Linux Counterpart
 * Linux Error: 1000 - 1999
 * Xen Error: 2000 - 2999
 */
#define ERR_NO          -1
#define ERR_NOEXIST     2000
#define ERR_PARAM       2001
#define ERR_XENFAULT    2002
#define ERR_MCDFAULT    2003
#define ERR_NOMCD       2004
#define ERR_NOTMEM      2005

#ifndef MEM_ALIGN
#define MEM_ALIGN (sizeof(void *) * 2)
#endif

// TODO TO header

#define EXPORT // indicates code other modules are dependent upon
#define FORWARD
#define LOCAL   static

//#define MCD_LINEAR_PROBE
//#define MCD_HASH_BUCKETS
#define MCD_HASH_CHAINING

// If there is not get op for MCD_EXP_TIME, we will remove the entry
#define MCD_EXP_TIME 500 //sec

#define mspin_lock_init(l) { spin_lock_init(l);\
        /*(printk("lock_init %s (%d) \n", __FUNCTION__, __LINE__);*/ }

#define mspin_lock(l) { spin_lock(l);\
        /*printk("locked %s (%d) \n", __FUNCTION__, __LINE__);*/ }

#define mspin_unlock(l) { spin_unlock(l);\
        /*printk("unlocked %s (%d) \n", __FUNCTION__, __LINE__);*/ }

/* 
 * Main Structure for mcd
 */
typedef struct g_mcd_data {
    spinlock_t lock;

    //uint32_t tot_nr_pages;
    uint32_t pri_nr_pages;
    uint32_t shr_nr_pages;
    // NB. tot_nr_pages = pri_nr_pages + shr_nr_pages;

    uint64_t pri_nr_bytes;
    uint64_t shr_nr_bytes;
    // NB. tot_nr_bytes = pri_nr_bytes + shr_nr_bytes;

    uint32_t pri_nr_keys;
    uint32_t shr_nr_keys;
} g_mcd_data_t ;

// TODO lock protection: each struct has a lock
LOCAL g_mcd_data_t mcd_data;
uint64_t max_mem_bytes; // maximum memory size

uint64_t current_free_memory_bytes; // current system memory

int cache_mode = 0; // 0: shared, 1: private
int replacement_mode = 0; // 0: slow, 1: fast

#define TO_MB(_a)       ((_a) / 1000000)
#define TOT_USED_MB()   ((mcd_data.pri_nr_bytes + mcd_data.shr_nr_bytes)/1000000)
#define TOT_USED_BYTES()   ((mcd_data.pri_nr_bytes + mcd_data.shr_nr_bytes))

LOCAL spinlock_t g_mcd_domain_list_lock; 
LOCAL struct list_head g_mcd_domain_list;

LOCAL spinlock_t g_mcd_shared_hash_lock;
#ifdef MCD_LINEAR_PROBE
LOCAL struct list_head g_mcd_shared_hash_list;
#elif defined(MCD_HASH_CHAINING)
#define MAX_LIST_HEAD   100
LOCAL struct list_head g_mcd_shared_hash_list[MAX_LIST_HEAD];
#endif
// TODO lock protection

/* 
 * Initial Flags
 */
EXPORT bool_t __read_mostly use_mcd_flag = 0;
boolean_param("mcd", use_mcd_flag);

EXPORT bool_t __read_mostly mcd_enabled_flag = 0;

/*
 * Function definition due to sequence
 */
LOCAL inline void mcd_copy_to_guest_buf_offset(mcd_va_t buf, int off,
                                           char *mcdbuf, int len);
LOCAL int shared_freeup_bytes(uint64_t amount);
LOCAL int private_freeup_bytes(uint64_t amount, domid_t domid);
LOCAL void dom_usage_update(mcd_domain_t *dom);
LOCAL uint64_t get_free_mem_size(void);
LOCAL uint64_t get_tot_mem_size(void);

/*
 * SSD Test Methods (TODO this is temporary function to test)
 */
#define N_TO_U(_n)    (_n / 1000UL)
uint64_t time_elapsed_us(uint64_t usec)
{
    return N_TO_U(NOW()) - usec;
}

void usleep(uint64_t usec)
{
    uint64_t cur_time = N_TO_U(NOW());
    uint32_t temp = 0;

    if ( usec == 0 ) return;

    while ( 1 ) {
        temp ++;
        if ( time_elapsed_us(cur_time) >= usec ) {
            break;
        }
    }

    //printk("usec = %ld, sleep = %ld \n", usec, time_elapsed_us(cur_time));
}

uint32_t rand_val(int seed)
{
    const long  a =      16807;  // Multiplier
    const long  m = 2147483647;  // Modulus
    const long  q =     127773;  // m div a
    const long  r =       2836;  // m mod a
    static long x =          0;  // Random int value
    long        x_div_q;         // x divided by q
    long        x_mod_q;         // x modulo q
    long        x_new;           // New x value

    // Set the seed if argument is non-zero and then return zero
    if (seed > 0)
    {
        x = seed;
        return(0.0);
    }

    // RNG using integer arithmetic
    x_div_q = x / q;
    x_mod_q = x % q;
    x_new = (a * x_mod_q) - (r * x_div_q);
    if (x_new > 0)
        x = x_new;
    else
        x = x_new + m;

    return (int)x;
}

/*
 * Xen Memory Management Functions
 */
LOCAL inline void *mcd_malloc(uint64_t size, uint64_t align, domid_t domid)
{
    uint64_t max_bytes = (max_mem_bytes == 0) ? get_tot_mem_size() : max_mem_bytes;

#define SAFETY_BYTES 1000

    if ( size == 0 ) return NULL;

    if ( (TOT_USED_BYTES() + size) >= max_bytes ) {
        if ( cache_mode == 0 ) {
            shared_freeup_bytes(size + SAFETY_BYTES);
        } else {
            private_freeup_bytes(size + SAFETY_BYTES, domid);
        }
    }

    return _xmalloc(size,align);
}

LOCAL int attach_mcd(mcd_domain_t *mcd_dom, domid_t domid) 
{
    struct domain *d = rcu_lock_domain_by_id(domid);

    if ( d == NULL )
        return -ERR_XENFAULT;

    if ( !d->is_dying ) {
        d->mcd = (void*)mcd_dom;
        mspin_lock(&mcd_dom->lock);
        mcd_dom->domain = d;
        mspin_unlock(&mcd_dom->lock);
    }

    rcu_unlock_domain(d);

    return -ERR_NO;
}

LOCAL void mcd_free(mcd_domain_t *d)
{
    if ( d == NULL ) return;

    mspin_lock(&d->lock);
    list_del(&d->mcd_list);
    mspin_unlock(&d->lock);

    xfree((void*)d);
}

LOCAL void remove_hashT(mcd_hashT_t *mh)
{
    if ( mh == NULL ) return;

    mspin_lock(&mh->lock);
    list_del(&mh->hash_list);
    mspin_unlock(&mh->lock);

    xfree((void*)mh);
}

LOCAL uint64_t p_remove_hashT(mcd_domain_t *dom, mcd_hashT_t *mh)
{
    uint64_t bytes = 0;

    if ( mh == NULL ) return 0;

    mspin_lock(&mcd_data.lock);
    mcd_data.pri_nr_pages -= mh->nr_pages;
    mcd_data.pri_nr_bytes -= mh->nr_bytes;
    mcd_data.pri_nr_keys --;
    mspin_unlock(&mcd_data.lock);

    if ( dom != NULL ) {
        mspin_lock(&dom->lock);
        dom->nr_used_pages -= mh->nr_pages;
        dom->nr_used_bytes -= mh->nr_bytes;
        mspin_unlock(&dom->lock);
    }

    bytes = mh->nr_bytes;

    remove_hashT(mh);
    dom_usage_update(dom);

    return bytes;
}

LOCAL uint64_t s_remove_hashT(mcd_hashT_t *mh)
{
    uint64_t bytes = 0;

    if ( mh == NULL ) return 0;

    mspin_lock(&mcd_data.lock);
    mcd_data.shr_nr_pages -= mh->nr_pages;
    mcd_data.shr_nr_bytes -= mh->nr_bytes;
    mcd_data.shr_nr_keys --;
    mspin_unlock(&mcd_data.lock);

    bytes = mh->nr_bytes;

    remove_hashT(mh);

    return bytes;
}

/*
 * Logging Functions
 */
LOCAL int mcdc_stat_display(mcd_va_t buf, int off, uint32_t len, bool_t use_long)
{
    char info[BSIZE];
    int n = 0, sum = 0;

    n += scnprintf(info, BSIZE, "tot_bytes_used:%ld (%ld:%ld),tot_bytes_free:%ld,nr_keys:%d\n",
                                mcd_data.shr_nr_bytes + mcd_data.pri_nr_bytes, 
                                mcd_data.shr_nr_bytes, mcd_data.pri_nr_bytes, 
                                get_free_mem_size(),
                                (cache_mode == 0) ? mcd_data.shr_nr_keys : mcd_data.pri_nr_keys);

    /*
    n += scnprintf(info, BSIZE, "tot_pages:%d,pri_pages:%d,shr_pages:%d,"
                                "tot_bytes_used:%ld,tot_bytes_free:%ld,pri_bytes:%ld,shr_bytes:%ld,"
                                "nr_pri_keys:%d,nr_shr_keys:%d\n",
                                mcd_data.pri_nr_pages + mcd_data.shr_nr_pages, 
                                mcd_data.pri_nr_pages,
                                mcd_data.shr_nr_pages,
                                mcd_data.pri_nr_bytes + mcd_data.shr_nr_bytes, 
                                current_free_memory_bytes,
                                mcd_data.pri_nr_bytes, 
                                mcd_data.shr_nr_bytes, 
                                mcd_data.pri_nr_keys,
                                mcd_data.shr_nr_keys);
    */

    if ( sum + n >= len )
        return sum;

    mcd_copy_to_guest_buf_offset(buf, off+sum, info, n+1);
    sum += n;

    return sum;
}

LOCAL int mcdc_domain_display(mcd_domain_t *mcd_dom, mcd_va_t buf, 
                               int off, uint32_t len, bool_t use_long)
{
    char info[BSIZE];
    int n = 0, sum = 0;
    struct domain *dom = domptr(mcd_dom);

    n += scnprintf(info, BSIZE, "tot:%d,max:%d,shr:%d,xenheap:%d,nr_avail:%d\n",
                                dom->tot_pages, dom->max_pages, dom->shr_pages.counter, 
                                dom->xenheap_pages, mcd_dom->nr_avail
                                );

    if ( sum + n >= len )
        return sum;

    mcd_copy_to_guest_buf_offset(buf, off+sum, info, n+1);
    sum += n;

    // more prints with the same format

    return sum;
}

enum display {
    DISPLAY_DOMAIN_STATS
};

LOCAL int mcdc_header(mcd_va_t buf, int off, uint32_t len)
{
    char info[BSIZE];
    int n = 0, sum = 0;
    char *str = NULL;

    str = (cache_mode == 0) ? "shared" : "private";
    n += scnprintf(info, BSIZE, "Memcached Statistics (%s mode) \n", str);

    if ( sum + n >= len )
        return sum;

    mcd_copy_to_guest_buf_offset(buf, off+sum, info, n+1);
    sum += n;

    return sum;
}

LOCAL int mcdc_domain_header(mcd_domain_t *dom, mcd_va_t buf, int off, uint32_t len)
{
    char info[BSIZE];
    int n = 0, sum = 0;

    //n += scnprintf(info, BSIZE, "dom:%d,weight:%d,ratio:%d,avail:%d,used:%d,bytes:%ld,curr:%d,", 
    //            dom->domid, dom->weight, dom->ratio, dom->nr_avail, dom->nr_used_pages, dom->nr_used_bytes, dom->curr);

    n += scnprintf(info, BSIZE, "dom:%d,weight:%d,ratio:%d,bytes:%ld,curr:%d,", 
                dom->domid, dom->weight, dom->ratio, dom->nr_used_bytes, dom->curr);

    if ( sum + n >= len )
        return sum;

    mcd_copy_to_guest_buf_offset(buf, off+sum, info, n+1);
    sum += n;

    return sum;
}

LOCAL int mcdc_shared_header(mcd_va_t buf, int off, uint32_t len)
{
    char info[BSIZE];
    int n = 0, sum = 0;

    n += scnprintf(info, BSIZE, "shared:,");

    if ( sum + n >= len )
        return sum;

    mcd_copy_to_guest_buf_offset(buf, off+sum, info, n+1);
    sum += n;

    return sum;
}

/*
 * Hash Functions: Private vs. Shared
 */
/*
LOCAL void mcd_printop(mcd_arg_t *op) 
{
    printk("option:%d, key_size:%d, val_size:%d, ", 
            op->option, 
            op->key_size,
            op->val_size);

    // key
    {
        // TODO : this needs to be moved to persistent location
        char buf[op->key_size];
        int i;

        //raw_copy_from_guest(buf, guest_handle_cast(op->key, void), op->key_size); // TODO confirm
        copy_from_guest(buf, guest_handle_cast(op->key, void), op->key_size);

        for(i = 0; i < op->key_size; i++) {
            printk("%c", buf[i]);
        }
    }
    printk(", ");

    // value
    {
        // TODO : this needs to be moved to persistent location
        char buf[op->val_size];
        int i;

        //raw_copy_from_guest(buf, guest_handle_cast(op->val, void), op->value_size); // TODO confirm
        copy_from_guest(buf, guest_handle_cast(op->val, void), op->val_size);

        for(i = 0; i < op->val_size; i++) {
            printk("%c", buf[i]);
        }
    }
    printk("\n");
}
*/

LOCAL int mcdc_hashT_display(mcd_hashT_t *data, mcd_va_t buf, int off, uint32_t len)
{
    char info[BSIZE];
    int n = 0, sum = 0, i = 0;
    s_time_t now = NOW();

    n += scnprintf(info, BSIZE, "nr_pages:%d,ref:%d,passed_time:%"PRIx64",hash:%d,key_size:%d,val_size:%d,key:",
                                data->nr_pages, data->ref, (uint64_t)(now - data->put_time), data->hash, data->key_size, 
                                data->val_size);

    for(i = 0; i < data->key_size; i++) {
        n += scnprintf(info+n, BSIZE-n, "%c", data->key_val[i]);
    }

    n += scnprintf(info+n, BSIZE-n, "\n");

    if ( sum + n >= len )
        return sum;

    mcd_copy_to_guest_buf_offset(buf, off+sum, info, n+1);
    sum += n;

    // more prints with the same format

    return sum;
}

/* superFastHash */
#define get16bits(d) (*((const uint16_t *) (d)))
LOCAL __attribute__ ((unused)) uint32_t getHash (const char * data, int len) 
{
    uint32_t hash = len, tmp; 
    int rem; 
                     
    if (len <= 0 || data == NULL) return 0;
                     
    rem = len & 3; 
    len >>= 2;   
                     
    for (;len > 0; len--) {
        hash += get16bits (data);
        tmp = (get16bits (data+2) << 11) ^ hash;
        hash = (hash << 16) ^ tmp; 
        data += 2*sizeof (uint16_t);
        hash += hash >> 11;
    }                
                     
    switch (rem) {
    case 3: 
        hash += get16bits (data);
        hash ^= hash << 16;
        hash ^= data[sizeof (uint16_t)] << 18;
        hash += hash >> 11;
        break;
    case 2: 
        hash += get16bits (data);
        hash ^= hash << 11;
        hash += hash >> 17;
        break;
    case 1: hash += *data;
        hash ^= hash << 10;
        hash += hash >> 1;
        break;
    }                
                     
    hash ^= hash << 3;
    hash += hash >> 5;
    hash ^= hash << 4;
    hash += hash >> 17;
    hash ^= hash << 25;
    hash += hash >> 6;

    return hash; 
}

/* 
 * Keep in Mind: only here allocate memory 
 * So, keep track of my much memory is used
 */
LOCAL mcd_hashT_t *create_hashT(mcd_arg_t *op, domid_t domid)
{
    uint64_t size = sizeof(mcd_hashT_t) + op->key_size + op->val_size;
    mcd_hashT_t *mh = NULL;

    if ( (size % MEM_ALIGN) > 0 ) {
        size += (MEM_ALIGN - (size % MEM_ALIGN));
    }

    // TODO I may need to think this alignment again
    mh = (mcd_hashT_t *) mcd_malloc(size, 0, domid);

    if ( mh == NULL ) {
        printk("mcd_malloc failed to create_hashT size = %ld \n", size);
    } else {
        mspin_lock_init(&mh->lock);

        mh->nr_pages = (size / PAGE_SIZE) + ((size % PAGE_SIZE > 0) ? 1:0);
        mh->nr_bytes = size;
        mh->ref = 0;
        mh->put_time = NOW(); // ns
        mh->key_size = op->key_size;
        mh->val_size = op->val_size;

        mh->key_val = ((char*)mh) + sizeof(mcd_hashT_t); // TODO confirm

        copy_from_guest(mh->key_val, guest_handle_cast(op->key, void), op->key_size);
        copy_from_guest((mh->key_val + op->key_size),
                        guest_handle_cast(op->val, void), op->val_size);

        // Copy value size back to user space for confirmation
        copy_to_guest(guest_handle_cast(op->r_val_size, void), &mh->val_size, sizeof(uint32_t));

        mh->hash = getHash(mh->key_val, op->key_size);
    }

    return mh;
}

/*
 * Free up memory (caching algorithm)
 */

LOCAL uint64_t get_free_mem_size(void)
{
    uint64_t used_bytes = mcd_data.pri_nr_bytes + mcd_data.shr_nr_bytes;
    uint64_t set_bytes = (max_mem_bytes <= 0) ? ((uint64_t)avail_domheap_pages() * PAGE_SIZE) : (max_mem_bytes);

    //return (used_bytes <= set_bytes) ? (set_bytes - used_bytes) : 0;
    return (max_mem_bytes <= 0) ? set_bytes : (set_bytes - used_bytes);
}

LOCAL uint64_t get_tot_mem_size(void)
{
    uint64_t tot_bytes = (mcd_data.pri_nr_bytes + mcd_data.shr_nr_bytes + ((uint64_t)avail_domheap_pages() * PAGE_SIZE));
    if ( max_mem_bytes == 0 ) {
        return tot_bytes;
    } else {
        return (tot_bytes <= (max_mem_bytes)) ? tot_bytes : max_mem_bytes;
    }
}

/*
LOCAL uint64_t get_used_mem_size(void)
{
    return (get_tot_mem_size() - get_free_mem_size());
}
*/

LOCAL void dom_usage_update(mcd_domain_t *dom)
{
    uint32_t ratio;

    if ( dom == NULL ) {
        return;
    }

    ratio = (uint32_t) ((dom->nr_used_bytes * 100ULL) / get_tot_mem_size());

    mspin_lock(&dom->lock);
    dom->curr = (dom->ratio <= 0) ? 0 : (uint32_t)((ratio * 100) / dom->ratio);
    mspin_unlock(&dom->lock);
}

LOCAL void dom_usage_update_all(void)
{
    mcd_domain_t* dom;

    current_free_memory_bytes = get_free_mem_size();
    list_for_each_entry(dom, &g_mcd_domain_list, mcd_list) {
        if ( dom->domid == 0 ) continue;
        dom_usage_update(dom);
    }
}

LOCAL void dom_weight_update(void)
{
    mcd_domain_t*   dom;

    if ( total_weight <= 0 ) {
        total_weight = 0;
        return;
    }

    list_for_each_entry(dom, &g_mcd_domain_list, mcd_list) {
        mspin_lock(&dom->lock);
        dom->ratio = (dom->weight * 100)/total_weight;
        mspin_unlock(&dom->lock);
    }
}

LOCAL int shared_freeup_bytes(uint64_t amount)
{
    s_time_t now = NOW();

    mcd_hashT_t     *curr;
    mcd_hashT_t     *latedel;

    s_time_t maxtime;
    uint32_t minref;

#ifdef MCD_LINEAR_PROBE
    uint64_t maxloop = 0;
#endif
    uint64_t tot = 0;
    int i;

    //printk("%ld MB Freeup\n", amount);

#define MAX_LINEAR_PROBE 1000

#ifdef MCD_LINEAR_PROBE

    while ( maxloop > MAX_LINEAR_PROBE ) {
        maxtime = 0;
        minref = (((uint32_t)~0ull>>1));
        latedel = NULL;

        list_for_each_entry(curr, &g_mcd_shared_hash_list, hash_list) {
            if ( latedel != NULL ) { 
                tot += s_remove_hashT(latedel);
                //printk("shared freed = %d\n", tot);
                latedel = NULL;
            }

            // TO Policy : TODO check time 
            if ( (now - curr->put_time)/1000000000 > MCD_EXP_TIME ) {
                latedel = curr;
                continue;
            }

            // Find LRU
            if ( (now - curr->put_time) > maxtime ) {
                maxtime = now - curr->put_time;
                latedel = curr;
            }

            // Find LFU
            if ( curr->ref < minref ) {
                minref = curr->ref;
                latedel = curr;
            }
        }

        if ( tot > amount ) return -ERR_NO;
        maxloop ++;
    }

#elif defined(MCD_HASH_CHAINING)

    if ( replacement_mode == 0 ) {
        int visited = 0;
        while ( tot < amount ) {
            for(i = 0; i < MAX_LIST_HEAD; i++) {
                struct list_head *hash = &g_mcd_shared_hash_list[i];

                maxtime = 0;
                minref = (((uint32_t)~0ull>>1));
                latedel = NULL;

                list_for_each_entry(curr, hash, hash_list) {
                    if ( latedel != NULL ) { 
                        tot += s_remove_hashT(latedel);
                        visited = 1;
                        //printk("shared freed = %ld\n", tot);
                        if ( tot > amount ) return -ERR_NO;
                        latedel = NULL;
                    }

                    // TO Policy : TODO check time 
                    /*
                    if ( (now - curr->put_time)/1000000000 > MCD_EXP_TIME ) {
                        //printk("time : %ld - %ld = %ld > %d ? \n", now, curr->put_time, ((now - curr->put_time)/1000000000), MCD_EXP_TIME);
                        latedel = curr;
                        continue;
                    }
                    */

                    // Find LRU
                    if ( (now - curr->put_time) > maxtime ) {
                        maxtime = now - curr->put_time;
                        latedel = curr;
                    }

                    // Find LFU
                    if ( curr->ref < minref ) {
                        minref = curr->ref;
                        latedel = curr;
                    }
                }
                if ( latedel != NULL ) {
                    tot += s_remove_hashT(latedel);
                    visited = 1;
                    if ( tot > amount ) return -ERR_NO;
                    latedel = NULL;
                }
            }

            // escape loop for safety
            if ( visited == 0 )
                break;
            visited = 0;
        }

        // Force to suit the size when not enough
        if ( tot < amount ) {
            for(i = 0; i < MAX_LIST_HEAD; i++) {
                struct list_head *hash = &g_mcd_shared_hash_list[i];
                latedel = NULL;

                list_for_each_entry(curr, hash, hash_list) {
                    if ( latedel != curr ) { 
                        tot += s_remove_hashT(latedel);
                        latedel = NULL;

                        if ( tot > amount ) break;
                    }
                    latedel = curr;
                }
                if ( latedel != NULL ) { 
                    tot += s_remove_hashT(latedel);
                    latedel = NULL;
                }

                if ( tot > amount ) break;
            }
        }
    } else {
        // Force to suit the size
        if ( tot < amount ) {
            for(i = 0; i < MAX_LIST_HEAD; i++) {
                struct list_head *hash = &g_mcd_shared_hash_list[i];
                latedel = NULL;

                list_for_each_entry(curr, hash, hash_list) {
                    if ( latedel != curr ) { 
                        tot += s_remove_hashT(latedel);
                        latedel = NULL;

                        if ( tot > amount ) break;
                    }
                    latedel = curr;
                }
                if ( latedel != NULL ) { 
                    tot += s_remove_hashT(latedel);
                    latedel = NULL;
                }

                if ( tot > amount ) break;
            }
        }
    }
#endif

    return -ERR_NO;
}

// find a overuse dom and free from it

LOCAL int private_freeup_bytes(uint64_t amount, domid_t domid)
{
    // domid = 0 -> brutally free memory

    s_time_t now = NOW();

    mcd_hashT_t     *curr = NULL;
    mcd_hashT_t     *latedel = NULL;
    mcd_domain_t*   dom = NULL;

    s_time_t maxtime = 0;
    uint32_t minref = 0;

    uint64_t maxloop = 0;
    uint64_t tot = 0;

    //printk("%d MB Freeup\n", amount);

    dom_usage_update_all();

#define MCD_MAX_LOOP 1000

    if ( domid == 0 ) { // brutally free up
        list_for_each_entry(dom, &g_mcd_domain_list, mcd_list) {
            latedel = NULL;
            list_for_each_entry(curr, &dom->mcd_hash_list_head, hash_list) {
                if ( latedel != NULL ) { 
                    tot += p_remove_hashT(dom, latedel);
                    latedel = NULL;

                    if ( tot > amount ) break;
                }
                latedel = curr;
            }

            if ( latedel != NULL ) {
                tot += p_remove_hashT(dom, latedel);
                latedel = NULL;

                if ( tot > amount ) break;
            }
        }
    } else {
        uint32_t fair_rate = 0;

#define FAIR_RATE(dom) (dom->curr / (dom->ratio + 1))

        list_for_each_entry(dom, &g_mcd_domain_list, mcd_list) {
            if ( dom->domid == domid ) {
                fair_rate = FAIR_RATE(dom);
            }
        }

        printk("1. debug point\n");

        // Find others first for fair share
        if ( tot < amount ) {
            int on_flag;
            maxloop = 0;
            while ( maxloop < MCD_MAX_LOOP ) {
                on_flag = 0;
                list_for_each_entry(dom, &g_mcd_domain_list, mcd_list) {
                    if ( dom->domid == domid ) continue;
                    if ( FAIR_RATE(dom) > fair_rate ) {
                        maxtime = 0;
                        minref = (((uint32_t)~0ull>>1));
                        latedel = NULL;

                        list_for_each_entry(curr, &dom->mcd_hash_list_head, hash_list) {
                            if ( latedel != NULL ) { 
                                tot += p_remove_hashT(dom, latedel);
                                latedel = NULL;

                                if ( tot >= amount ) break;
                            }

                            // TO Policy : TODO check time 
                            if ( (now - curr->put_time)/1000000000 > MCD_EXP_TIME ) {
                                latedel = curr;
                                continue;
                            }

                            // Find LRU
                            if ( (now - curr->put_time) > maxtime ) {
                                maxtime = now - curr->put_time;
                                latedel = curr;
                            }

                            // Find LFU
                            if ( curr->ref < minref ) {
                                minref = curr->ref;
                                latedel = curr;
                            }
                        }

                        if ( latedel != NULL ) {
                            tot += p_remove_hashT(dom, latedel);
                            latedel = NULL;

                            if ( tot >= amount ) break;
                        }

                        on_flag = 1;
                    }
                    if ( tot >= amount ) break;
                }
                if ( tot >= amount ) break;
                if ( on_flag == 0 ) break; // nothing to check
                maxloop++;
            }
        }

        printk("2. debug point\n");

        // self when > 100
        if ( tot < amount ) {
            list_for_each_entry(dom, &g_mcd_domain_list, mcd_list) {
                if ( dom->domid == domid && dom->curr > 100 ) {
                    maxtime = 0;
                    minref = (((uint32_t)~0ull>>1));
                    latedel = NULL;

                    maxloop = 0;
                    while ( maxloop < MCD_MAX_LOOP ) {

                        if ( dom->curr < 100 ) break;

                        list_for_each_entry(curr, &dom->mcd_hash_list_head, hash_list) {
                            if ( latedel != NULL ) { 
                                tot += p_remove_hashT(dom, latedel);
                                latedel = NULL;

                                if ( tot >= amount ) break;
                            }

                            // TO Policy : TODO check time 
                            if ( (now - curr->put_time)/1000000000 > MCD_EXP_TIME ) {
                                latedel = curr;
                                continue;
                            }

                            // Find LRU
                            if ( (now - curr->put_time) > maxtime ) {
                                maxtime = now - curr->put_time;
                                latedel = curr;
                            }

                            // Find LFU
                            if ( curr->ref < minref ) {
                                minref = curr->ref;
                                latedel = curr;
                            }
                        }

                        if ( latedel != NULL ) {
                            tot += p_remove_hashT(dom, latedel);
                            latedel = NULL;

                            if ( tot >= amount ) break;
                        }

                        if ( tot >= amount ) break;
                        maxloop ++;
                    } // while

                    break; // only one self exists
                } // if
            } // list_for_each_entry
        } // if

        printk("3. debug point\n");

        // Others check when > 100
        if ( tot < amount ) {
            int on_flag;
            maxloop = 0;
            while ( maxloop < MCD_MAX_LOOP ) {
                on_flag = 0;
                list_for_each_entry(dom, &g_mcd_domain_list, mcd_list) {
                    if ( dom->domid == domid ) continue;
                    if ( dom->curr > 100 ) {
                        maxtime = 0;
                        minref = (((uint32_t)~0ull>>1));
                        latedel = NULL;

                        list_for_each_entry(curr, &dom->mcd_hash_list_head, hash_list) {
                            if ( latedel != NULL ) { 
                                tot += p_remove_hashT(dom, latedel);
                                latedel = NULL;

                                if ( tot >= amount ) break;
                            }

                            // TO Policy : TODO check time 
                            if ( (now - curr->put_time)/1000000000 > MCD_EXP_TIME ) {
                                latedel = curr;
                                continue;
                            }

                            // Find LRU
                            if ( (now - curr->put_time) > maxtime ) {
                                maxtime = now - curr->put_time;
                                latedel = curr;
                            }

                            // Find LFU
                            if ( curr->ref < minref ) {
                                minref = curr->ref;
                                latedel = curr;
                            }
                        }

                        if ( latedel != NULL ) {
                            tot += p_remove_hashT(dom, latedel);
                            latedel = NULL;

                            if ( tot >= amount ) break;
                        }

                        on_flag = 1;
                    }
                    if ( tot >= amount ) break;
                }
                if ( tot >= amount ) break;
                if ( on_flag == 0 ) break; // nothing to check
                maxloop++;
            }
        }

        printk("4. debug point\n");

        // Self release
        if ( tot < amount ) {
            list_for_each_entry(dom, &g_mcd_domain_list, mcd_list) {
                if ( dom->domid == domid ) {
                    maxtime = 0;
                    minref = (((uint32_t)~0ull>>1));
                    latedel = NULL;

                    maxloop = 0;
                    while ( maxloop < MCD_MAX_LOOP ) {

                        if ( dom->curr <= 0 ) break;

                        list_for_each_entry(curr, &dom->mcd_hash_list_head, hash_list) {
                            if ( latedel != NULL ) { 
                                tot += p_remove_hashT(dom, latedel);
                                latedel = NULL;

                                if ( tot >= amount ) break;
                            }

                            // TO Policy : TODO check time 
                            if ( (now - curr->put_time)/1000000000 > MCD_EXP_TIME ) {
                                latedel = curr;
                                continue;
                            }

                            // Find LRU
                            if ( (now - curr->put_time) > maxtime ) {
                                maxtime = now - curr->put_time;
                                latedel = curr;
                            }

                            // Find LFU
                            if ( curr->ref < minref ) {
                                minref = curr->ref;
                                latedel = curr;
                            }
                        }

                        if ( latedel != NULL ) {
                            tot += p_remove_hashT(dom, latedel);
                            latedel = NULL;

                            if ( tot >= amount ) break;
                        }

                        if ( tot >= amount ) break;

                        maxloop ++;
                    } // while

                    break;
                } // if
            } // list_for_each_entry
        } // if

        printk("5. debug point\n");

        // Self brutally release
        if ( tot < amount ) {
            maxloop = 0;
            while ( maxloop < MCD_MAX_LOOP*100 ) {

                if ( dom->curr <= 0 ) break;

                list_for_each_entry(curr, &dom->mcd_hash_list_head, hash_list) {
                    if ( latedel != NULL ) { 
                        tot += p_remove_hashT(dom, latedel);
                        latedel = NULL;

                        if ( tot >= amount ) break;
                    }
                    latedel = curr;
                }

                if ( latedel != NULL ) {
                    tot += p_remove_hashT(dom, latedel);
                    latedel = NULL;

                    if ( tot >= amount ) break;
                }

                if ( tot >= amount ) break;

                maxloop ++;
            } // while
        }

        printk("6. debug point\n\n");
    }

    return -ERR_NO;
}

LOCAL mcd_hashT_t *find_hashT(int option, struct list_head *head, uint32_t hash, char* key, uint32_t key_size)
{
    mcd_hashT_t *curr;
    mcd_hashT_t *found = NULL;

    list_for_each_entry(curr, head, hash_list) {
        if ( curr->hash == hash ) {
            int i;
            int is_same = 1;
            for(i = 0; i < key_size; i++) {
                if ( curr->key_val[i] != key[i] ) {
                    is_same = 0;
                    break;
                }
            }

            if ( is_same ) {
                found = curr;
                break;
            } 
        }
    }

    return found;
}

/* 
 * Private 
 */
LOCAL int p_mcd_check(mcd_domain_t *mcd_dom, mcd_arg_t *op)
{
    mcd_hashT_t *mh;
    char key[op->key_size];
    uint32_t hash;
    uint32_t error = -ERR_NOEXIST;

    copy_from_guest(key, guest_handle_cast(op->key, void), op->key_size);
    hash = getHash(key, op->key_size);

    mh = find_hashT(MCDOPT_private, &mcd_dom->mcd_hash_list_head, hash, key, op->key_size);

    if ( mh == NULL ) {
        copy_to_guest(guest_handle_cast(op->r_val_size, void), &error, sizeof(uint32_t));
    } else {
        copy_to_guest(guest_handle_cast(op->r_val_size, void), &mh->val_size, sizeof(uint32_t));
    }

    return -ERR_NO;
}

LOCAL int p_mcd_remove(mcd_domain_t *mcd_dom, mcd_arg_t *op)
{
    mcd_hashT_t *mh;
    char key[op->key_size];
    uint32_t hash;

    copy_from_guest(key, guest_handle_cast(op->key, void), op->key_size);
    hash = getHash(key, op->key_size);

    mh = find_hashT(MCDOPT_private, &mcd_dom->mcd_hash_list_head, hash, key, op->key_size);
    if ( mh == NULL ) return -ERR_MCDFAULT;

    p_remove_hashT(mcd_dom, mh);

    return -ERR_NO;
}

LOCAL int p_mcd_put(mcd_domain_t *mcd_dom, mcd_arg_t *op)
{
    mcd_hashT_t *mh;
    char key[op->key_size];
    uint32_t hash = 0;

    copy_from_guest(key, guest_handle_cast(op->key, void), op->key_size);
    hash = getHash(key, op->key_size);

    mh = find_hashT(MCDOPT_private, &mcd_dom->mcd_hash_list_head, hash, key, op->key_size);
    if ( mh != NULL ) p_remove_hashT(mcd_dom, mh);

    mh = create_hashT(op, mcd_dom->domid);
    if ( mh == NULL ) return -ERR_MCDFAULT;

    mspin_lock(&mcd_dom->lock);
    list_add_tail(&mh->hash_list, &mcd_dom->mcd_hash_list_head);
    mcd_dom->nr_used_pages += mh->nr_pages;
    mcd_dom->nr_used_bytes += mh->nr_bytes;
    mspin_unlock(&mcd_dom->lock);

    mspin_lock(&mcd_data.lock);
    mcd_data.pri_nr_pages += mh->nr_pages;
    mcd_data.pri_nr_bytes += mh->nr_bytes;
    mcd_data.pri_nr_keys ++;
    mspin_unlock(&mcd_data.lock);

    copy_to_guest(guest_handle_cast(op->r_val_size, void), &mh->val_size, sizeof(uint32_t));

    return -ERR_NO;
}

LOCAL int p_mcd_get(mcd_domain_t *mcd_dom, mcd_arg_t *op)
{
    mcd_hashT_t *mh;
    char key[op->key_size];
    uint32_t hash;

    copy_from_guest(key, guest_handle_cast(op->key, void), op->key_size);
    hash = getHash(key, op->key_size);

    mh = find_hashT(MCDOPT_private, &mcd_dom->mcd_hash_list_head, hash, key, op->key_size);
    if ( mh == NULL ) return -ERR_MCDFAULT;

    mspin_lock(&mh->lock);
    mh->ref ++;
    mh->put_time = NOW();
    mspin_unlock(&mh->lock);

    if ( mh->val_size > op->val_size ) return -ERR_MCDFAULT;
    copy_to_guest(guest_handle_cast(op->r_val_size, void), &mh->val_size, sizeof(uint32_t));
    copy_to_guest(guest_handle_cast(op->val, void), (mh->key_val + mh->key_size), mh->val_size);

    return -ERR_NO;
}

LOCAL int p_mcd_flush(mcd_domain_t *mcd_dom, mcd_arg_t *op)
{
    mcd_hashT_t *mh;
    mcd_hashT_t *prev = NULL;

    if ( mcd_dom == NULL ) return -ERR_NOEXIST;

    // Hash Table Memory Cleanup
    list_for_each_entry(mh, &mcd_dom->mcd_hash_list_head, hash_list) {
        if ( prev == NULL ) { // first item
            prev = mh;
            continue;
        }
        p_remove_hashT(mcd_dom, prev);
        prev = mh;
    }

    if ( prev != NULL )  // last item
        p_remove_hashT(mcd_dom, prev);

    return -ERR_NO;
}

/* 
 * Shared 
 */
LOCAL int s_mcd_check(mcd_arg_t *op)
{
    mcd_hashT_t *mh;
    char key[op->key_size];
    uint32_t hash;
    uint32_t error = -ERR_NOEXIST;

    copy_from_guest(key, guest_handle_cast(op->key, void), op->key_size);
    hash = getHash(key, op->key_size);

#ifdef MCD_LINEAR_PROBE
    mh = find_hashT(MCDOPT_shared, &g_mcd_shared_hash_list, hash, key, op->key_size);
#elif defined(MCD_HASH_CHAINING)
    mh = find_hashT(MCDOPT_shared, &g_mcd_shared_hash_list[hash % MAX_LIST_HEAD], hash, key, op->key_size);
#endif
    if ( mh == NULL ) {
        copy_to_guest(guest_handle_cast(op->r_val_size, void), &error, sizeof(uint32_t));
    } else {
        copy_to_guest(guest_handle_cast(op->r_val_size, void), &mh->val_size, sizeof(uint32_t));
    }

    return -ERR_NO;
}

LOCAL int s_mcd_put(mcd_arg_t *op)
{
    mcd_hashT_t *mh;
    char key[op->key_size];
    uint32_t hash = 0;

    copy_from_guest(key, guest_handle_cast(op->key, void), op->key_size);
    hash = getHash(key, op->key_size);

#ifdef MCD_LINEAR_PROBE
    mh = find_hashT(MCDOPT_shared, &g_mcd_shared_hash_list, hash, key, op->key_size);
#elif defined(MCD_HASH_CHAINING)
    mh = find_hashT(MCDOPT_shared, &g_mcd_shared_hash_list[hash % MAX_LIST_HEAD], hash, key, op->key_size);
#endif
    if ( mh != NULL ) s_remove_hashT(mh);

    mh = create_hashT(op, 0);
    if ( mh == NULL ) return -ERR_MCDFAULT;

    mspin_lock(&g_mcd_shared_hash_lock);
#ifdef MCD_LINEAR_PROBE
    list_add_tail(&mh->hash_list, &g_mcd_shared_hash_list);
#elif defined(MCD_HASH_CHAINING)
    list_add_tail(&mh->hash_list, &g_mcd_shared_hash_list[hash % MAX_LIST_HEAD]);
#endif
    mspin_unlock(&g_mcd_shared_hash_lock);

    mspin_lock(&mcd_data.lock);
    mcd_data.shr_nr_pages += mh->nr_pages;
    mcd_data.shr_nr_bytes += mh->nr_bytes;
    mcd_data.shr_nr_keys ++;
    mspin_unlock(&mcd_data.lock);

    copy_to_guest(guest_handle_cast(op->r_val_size, void), &mh->val_size, sizeof(uint32_t));

    return -ERR_NO;
}

LOCAL int s_mcd_get(mcd_arg_t *op)
{
    mcd_hashT_t *mh;
    char key[op->key_size];
    uint32_t hash;

    copy_from_guest(key, guest_handle_cast(op->key, void), op->key_size);
    hash = getHash(key, op->key_size);

#ifdef MCD_LINEAR_PROBE
    mh = find_hashT(MCDOPT_shared, &g_mcd_shared_hash_list, hash, key, op->key_size);
#elif defined(MCD_HASH_CHAINING)
    mh = find_hashT(MCDOPT_shared, &g_mcd_shared_hash_list[hash % MAX_LIST_HEAD], hash, key, op->key_size);
#endif
    if ( mh == NULL ) return -ERR_MCDFAULT;

    mspin_lock(&mh->lock);
    mh->ref ++;
    mh->put_time = NOW();
    mspin_unlock(&mh->lock);

    if ( mh->val_size > op->val_size ) return -ERR_MCDFAULT;
    copy_to_guest(guest_handle_cast(op->r_val_size, void), &mh->val_size, sizeof(uint32_t));
    copy_to_guest(guest_handle_cast(op->val, void), (mh->key_val + mh->key_size), mh->val_size);

    return -ERR_NO;
}

LOCAL int s_mcd_remove(mcd_arg_t *op)
{
    mcd_hashT_t *mh;
    char key[op->key_size];
    uint32_t hash;

    copy_from_guest(key, guest_handle_cast(op->key, void), op->key_size);
    hash = getHash(key, op->key_size);

#ifdef MCD_LINEAR_PROBE
    mh = find_hashT(MCDOPT_shared, &g_mcd_shared_hash_list, hash, key, op->key_size);
#elif defined(MCD_HASH_CHAINING)
    mh = find_hashT(MCDOPT_shared, &g_mcd_shared_hash_list[hash % MAX_LIST_HEAD], hash, key, op->key_size);
#endif
    if ( mh == NULL ) return -ERR_MCDFAULT;

    s_remove_hashT(mh);

    return -ERR_NO;
}

LOCAL int s_mcd_flush(mcd_arg_t *op)
{
#ifdef MCD_LINEAR_PROBE
    mcd_hashT_t *mh;
    mcd_hashT_t *prev;

    // TODO check whether this is correct -> late free??
    prev = NULL;
    list_for_each_entry(mh, &g_mcd_shared_hash_list, hash_list) {
        if ( prev != NULL ) {
            s_remove_hashT(prev);
            prev = NULL;
        }
        prev = mh;
    }

    if ( prev != NULL ) // last item
        s_remove_hashT(prev);

#elif defined(MCD_HASH_CHAINING)
    mcd_hashT_t *mh;
    mcd_hashT_t *prev;
    int i;

    for(i = 0; i < MAX_LIST_HEAD; i++) {
        struct list_head *hash_list = &g_mcd_shared_hash_list[i];
        
        prev = NULL;
        list_for_each_entry(mh, hash_list, hash_list) {
            if ( prev == NULL ) { // first item
                prev = mh;
                continue;
            }
            s_remove_hashT(prev);
            prev = mh;
        }

        if ( prev != NULL ) // last item
            s_remove_hashT(prev);
    }

#endif

    return -ERR_NO;
}

LOCAL void s_mcd_flush_all(void)
{
    s_mcd_flush(NULL);
}

/*
 * Domain-related functions
 */
LOCAL void mcd_domain_free(mcd_domain_t *d)
{
    if( d == NULL ) return;

    p_mcd_flush(d, NULL);
    mcd_free(d);
}

LOCAL void p_mcd_flush_all(void)
{
    mcd_domain_t *dom;

    list_for_each_entry(dom, &g_mcd_domain_list, mcd_list) {
        if ( dom->domid == 0 ) continue;
        p_mcd_flush(dom, NULL);
    }
}

LOCAL inline mcd_domain_t *mcd_domain_from_domid(domid_t did)
{
    mcd_domain_t *c;
    struct domain *d = rcu_lock_domain_by_id(did);
    if (d == NULL)
        return NULL;

    c = (mcd_domain_t *)(d->mcd);
    rcu_unlock_domain(d);

    return c;
}

LOCAL inline mcd_domain_t *mcd_get_mcd_domain_from_current(void)
{
    return (mcd_domain_t*)(current->domain->mcd);
}

LOCAL inline domid_t mcd_get_domid_from_current(void)
{
    return current->domain->domain_id;
}

LOCAL inline struct domain *mcd_get_domain_from_current(void)
{
    return (current->domain);
}

LOCAL inline mcd_domain_t *find_mcd_dom(domid_t domid)
{
    mcd_domain_t*   dom;

    list_for_each_entry(dom, &g_mcd_domain_list, mcd_list) {
        if ( dom->domid == domid ) 
            break;
    }

    return dom;
}

LOCAL inline void mcd_copy_to_guest_buf_offset(mcd_va_t buf, int off,
                                           char *mcdbuf, int len)
{
    copy_to_guest_offset(buf, off, mcdbuf, len);
}

/*
 * Hypercall Operations Functions
 */
LOCAL int display_stats(domid_t dom_id, mcd_va_t buf, uint32_t len, bool_t use_long)
{
    mcd_domain_t*   dom;
    int off = 0;

    off += mcdc_header(buf, 0, len);
    off += mcdc_stat_display(buf, off, len-off, use_long);

    if( dom_id == MCD_DOM_ID_NULL ) {
        list_for_each_entry(dom, &g_mcd_domain_list, mcd_list) {
            off += mcdc_domain_header(dom, buf, off, len-off);
            off += mcdc_domain_display(dom, buf, off, len-off, use_long);

            // detail
            if ( use_long ) {
                mcd_hashT_t *curr;
                struct list_head *head;

                head = &dom->mcd_hash_list_head;
                list_for_each_entry(curr, head, hash_list) { // queue list
                    off += mcdc_hashT_display(curr, buf, off, len-off);
                }
            }
        }
    } else {
        // when error, return -1;
    }

    // detail for shared queue
    if ( use_long ) {
        mcd_hashT_t *curr;
        int i;

        off += mcdc_shared_header(buf, off, len-off);
#ifdef MCD_LINEAR_PROBE
        list_for_each_entry(curr, &g_mcd_shared_hash_list, hash_list) {
            off += mcdc_hashT_display(curr, buf, off, len-off);
        }
#elif defined(MCD_HASH_CHAINING)
        for(i = 0; i < MAX_LIST_HEAD; i++) {
            struct list_head *hash_list = &g_mcd_shared_hash_list[i];
            list_for_each_entry(curr, hash_list, hash_list) {
                off += mcdc_hashT_display(curr, buf, off, len-off);
            }
        }
#endif
    }

    return -ERR_NO;
}

LOCAL int set_memsize(int memsize, mcd_va_t buf, uint32_t len)
{
    char info[BSIZE];
    int n = 0;
    
    //s_time_t prev = NOW();

    max_mem_bytes = ((uint64_t)memsize) * 1000000ULL;
    //printk("mcd: max_mem_bytes is set %ld bytes, current = %ld \n", max_mem_bytes, TOT_USED_BYTES());
    if ( max_mem_bytes != 0 && TOT_USED_BYTES() > max_mem_bytes ) {
        if ( cache_mode == 0 ) {
            shared_freeup_bytes(TOT_USED_BYTES() - max_mem_bytes);
        } else {
            private_freeup_bytes(TOT_USED_BYTES() - max_mem_bytes, 0);
        }
    }

//printk("shared_freeup_bytes time = %"PRIx64" micro-secs \n", (uint64_t)((NOW() - prev)/1000000UL));

    n += scnprintf(info, BSIZE, "Maximum memory size is set to %d MB", memsize);
    mcd_copy_to_guest_buf_offset(buf, 0, info, n);

    dom_usage_update_all();

    return -ERR_NO;
}

LOCAL int set_weight(domid_t domid, uint32_t weight, mcd_va_t buf, uint32_t len)
{
    char info[BSIZE];
    int n = 0;
    mcd_domain_t *mcd_dom = find_mcd_dom(domid);

    if ( mcd_dom == NULL ) {
        printk("mcd_dom for %d is NULL\n", domid);
        return -1;
    }

//printk("domid = %d, weight = %d\n", domid, weight);

    total_weight += (weight - mcd_dom->weight);

    mspin_lock(&mcd_dom->lock);
    mcd_dom->weight = weight; 
    mspin_unlock(&mcd_dom->lock);

    dom_weight_update();
    
    n += scnprintf(info, BSIZE, "Weight of dom %d is set to %d", domid, weight);
    mcd_copy_to_guest_buf_offset(buf, 0, info, n);

    return -ERR_NO;
}

LOCAL int set_cache_mode(uint32_t which, uint32_t mode, mcd_va_t buf, uint32_t len)
{
    char info[BSIZE];
    char *str = NULL;
    uint32_t prev = cache_mode;
    int n = 0;

    switch ( which ) {
    case 0:
        cache_mode = mode;
        str = (mode == 0) ? "shared" : "private";

        if ( prev != cache_mode ) {
            (cache_mode == 0) ? p_mcd_flush_all() : s_mcd_flush_all();
        }

        break;
    case 1:
    default:
        replacement_mode = mode;
        str = (mode == 0) ? "slow" : "fast";
        break;
    }

    n += scnprintf(info, BSIZE, "Set to %s cache mode", str);
    mcd_copy_to_guest_buf_offset(buf, 0, info, n);

    return -ERR_NO;
}

LOCAL int set_ssd_param(uint32_t which, uint32_t param, mcd_va_t buf, uint32_t len)
{
    char info[BSIZE];
    int n = 0;

    switch ( which ) {
    case 0:
        ssd_r_latency_us = param;
        break;
    case 1:
        ssd_w_latency_us = param;
        break;
    case 2:
        ssd_usage_rate_percent = (param * 100) / (100 + param); // cache is 100%, normalized to 100
        break;
    default:
        break;
    }

    n += scnprintf(info, BSIZE, "Params is set to (%u, %u, %u)", ssd_r_latency_us, ssd_r_latency_us, ssd_usage_rate_percent);
    mcd_copy_to_guest_buf_offset(buf, 0, info, n);

    return -ERR_NO;
}

LOCAL long mcd_display(XEN_GUEST_HANDLE(mcd_display_stats_t) arg)
{
    mcd_display_stats_t disp;

    if( copy_from_guest(&disp, arg, 1) ) 
        return -ERR_XENFAULT;

    display_stats(disp.domid, disp.buf, disp.buf_len, disp.option);

    return -ERR_NO;
}

LOCAL long mcd_memsize(XEN_GUEST_HANDLE(mcd_memsize_t) arg)
{
    mcd_memsize_t memsize;

    if( copy_from_guest(&memsize, arg, 1) ) 
        return -ERR_XENFAULT;

    set_memsize(memsize.memsize, memsize.buf, memsize.buf_len);

    return -ERR_NO;
}

LOCAL long mcd_weight(XEN_GUEST_HANDLE(mcd_weight_t) arg)
{
    mcd_weight_t weight;

    if( copy_from_guest(&weight, arg, 1) ) 
        return -ERR_XENFAULT;

    set_weight(weight.domid, weight.weight, weight.buf, weight.buf_len);

    return -ERR_NO;
}

LOCAL long mcd_cache_mode(XEN_GUEST_HANDLE(mcd_cache_mode_t) arg)
{
    mcd_cache_mode_t mode;

    if( copy_from_guest(&mode, arg, 1) ) 
        return -ERR_XENFAULT;

    set_cache_mode(mode.which, mode.cache_mode, mode.buf, mode.buf_len);

    return -ERR_NO;
}

LOCAL long mcd_ssd_param(XEN_GUEST_HANDLE(mcd_ssd_param_t) arg)
{
    mcd_ssd_param_t param;

    if( copy_from_guest(&param, arg, 1) ) 
        return -ERR_XENFAULT;

    set_ssd_param(param.which, param.param, param.buf, param.buf_len);

    return -ERR_NO;
}

// fn = get, put, remove, check
#define mcd_sub_call(opt, fn, md, op) \
        ( (opt) == MCDOPT_private ) ? p_mcd_ ## fn ((md), (op)) : s_mcd_ ## fn ((op))

LOCAL long mcd_get(mcd_domain_t *mcd_dom, XEN_GUEST_HANDLE(mcd_arg_t) arg)
{
    mcd_arg_t op;

    if ( copy_from_guest(&op, arg, 1) ) {
        printk("copy_from_gest failed\n");
        return -ERR_XENFAULT;
    }

    return mcd_sub_call(op.option, get, mcd_dom, &op);
}

LOCAL long mcd_stat_get(mcd_domain_t *mcd_dom, XEN_GUEST_HANDLE(mcd_arg_t) arg)
{
    mcd_arg_t op;
    char buf[128];
    int size;

    if ( copy_from_guest(&op, arg, 1) ) {
        printk("copy_from_gest failed\n");
        return -ERR_XENFAULT;
    }

    // do here...
    size = snprintf(buf, 128, "%ld,%ld", get_tot_mem_size(), get_free_mem_size());

    printk("val_size = %d, size = %d, output = %s\n", op.val_size, size, buf);

    if ( size > op.val_size ) return -ERR_MCDFAULT;
    copy_to_guest(guest_handle_cast(op.r_val_size, void), &size, sizeof(uint32_t));
    copy_to_guest(guest_handle_cast(op.val, void), buf, size);

    return -ERR_NO;
}

LOCAL long mcd_put(mcd_domain_t *mcd_dom, XEN_GUEST_HANDLE(mcd_arg_t) arg)
{
    mcd_arg_t op;

    if ( copy_from_guest(&op, arg, 1) ) {
        printk("copy_from_gest failed\n");
        return -ERR_XENFAULT;
    }

    return mcd_sub_call(op.option, put, mcd_dom, &op);
}

LOCAL long mcd_remove(mcd_domain_t *mcd_dom, XEN_GUEST_HANDLE(mcd_arg_t) arg)
{
    mcd_arg_t op;

    if ( copy_from_guest(&op, arg, 1) ) {
        printk("copy_from_gest failed\n");
        return -ERR_XENFAULT;
    }

    return mcd_sub_call(op.option, remove, mcd_dom, &op);
}

LOCAL long mcd_check(mcd_domain_t *mcd_dom, XEN_GUEST_HANDLE(mcd_arg_t) arg)
{
    mcd_arg_t op;

    if ( copy_from_guest(&op, arg, 1) ) {
        printk("copy_from_gest failed\n");
        return -ERR_XENFAULT;
    }

    return mcd_sub_call(op.option, check, mcd_dom, &op);
}

LOCAL long mcd_flush(mcd_domain_t *mcd_dom, XEN_GUEST_HANDLE(mcd_arg_t) arg)
{
    mcd_arg_t op;

    if ( copy_from_guest(&op, arg, 1) ) {
        printk("copy_from_gest failed\n");
        return -ERR_XENFAULT;
    }

    return mcd_sub_call(op.option, flush, mcd_dom, &op);
}

/*
 * Exported Functions
 */ 
EXPORT long do_mcd_op(unsigned long cmd, XEN_GUEST_HANDLE(void) arg)
{
    mcd_domain_t *mcd_dom = mcd_get_mcd_domain_from_current();
    int rc = 0;

    if( !mcd_enabled_flag )
        return -ERR_NOMCD;

    if( mcd_dom != NULL && mcd_dom_is_dying(mcd_dom) )
        return -ERR_XENFAULT;

    /*
     * Memcached Operations
     */
    switch( cmd )
    {
    case XENMCD_cache_get:
        if ( (rand_val(0) % 100) <= ssd_usage_rate_percent ) usleep(ssd_r_latency_us);  // TODO test function
        rc = mcd_get(mcd_dom, guest_handle_cast(arg, mcd_arg_t));
        break;

    case XENMCD_stat_get:
        rc = mcd_stat_get(mcd_dom, guest_handle_cast(arg, mcd_arg_t));
        break;

    case XENMCD_cache_put:
        if ( (rand_val(0) % 100) <= ssd_usage_rate_percent ) usleep(ssd_w_latency_us); // TODO test function
        rc = mcd_put(mcd_dom, guest_handle_cast(arg, mcd_arg_t));
        break;

    case XENMCD_cache_remove:
        rc = mcd_remove(mcd_dom, guest_handle_cast(arg, mcd_arg_t));
        break;

    case XENMCD_cache_check:
    case XENMCD_cache_getsize:
        rc = mcd_check(mcd_dom, guest_handle_cast(arg, mcd_arg_t));
        break;

    case XENMCD_cache_flush:
        rc = mcd_flush(mcd_dom, guest_handle_cast(arg, mcd_arg_t));
        break;

    case XENMCD_display_stats:
        rc = mcd_display(guest_handle_cast(arg, mcd_display_stats_t));
        break;

    case XENMCD_memsize:
        rc = mcd_memsize(guest_handle_cast(arg, mcd_memsize_t));
        break;

    case XENMCD_weight:
        rc = mcd_weight(guest_handle_cast(arg, mcd_weight_t));
        break;

    case XENMCD_cache_mode:
        rc = mcd_cache_mode(guest_handle_cast(arg, mcd_cache_mode_t));
        break;

    // TODO TEST
    case XENMCD_ssd_param:
        rc = mcd_ssd_param(guest_handle_cast(arg, mcd_ssd_param_t));
        break;

    case XENMCD_hypercall_test:
        printk(KERN_INFO "hypercall success...\n");
        break;

    default:
        break;
    }

    // Other works
    switch( cmd )
    {
    // Proportion can be changed...
    case XENMCD_cache_put:
    case XENMCD_cache_remove:
    case XENMCD_cache_flush:
    case XENMCD_memsize:
    case XENMCD_weight:
        dom_usage_update_all();

        break;
    case XENMCD_cache_get:
    case XENMCD_cache_check:
    case XENMCD_cache_getsize:
    case XENMCD_display_stats:
    case XENMCD_hypercall_test:
    default:
        break;
    }


    return rc;
}

EXPORT int mcd_domain_create(domid_t domid)
{
    // TODO we use some of hypervisor heap memory... needs to take account
    mcd_domain_t *mcd_dom = mcd_malloc(sizeof(mcd_domain_t), __alignof__(mcd_domain_t), domid);

    if( mcd_dom == NULL ) {
        printk(KERN_INFO "mcd_malloc failed.. OOM for domain(%d)\n", domid);
        goto fail;
    }
    memset(mcd_dom, 0, sizeof(mcd_domain_t));

    mcd_dom->domid = domid;
    mspin_lock_init(&mcd_dom->lock);
    if( !attach_mcd(mcd_dom, domid) ) {
        printk(KERN_ERR "attach_mcd failed for dom(%d)\n", domid);
        goto fail;
    }

    mcd_dom->nr_avail = 0;
    mcd_dom->nr_used_pages = 0;
    mcd_dom->nr_used_bytes = 0;

    if ( domid > 0 ) {
        mcd_dom->weight = DOM_WEIGHT_DEFAULT;
        total_weight += DOM_WEIGHT_DEFAULT;
    } else {
        mcd_dom->weight = 0; // dom0
    }

    //printk("total_weight = %d\n", total_weight);

    mspin_lock(&g_mcd_domain_list_lock);
    list_add_tail(&mcd_dom->mcd_list, &g_mcd_domain_list);
    INIT_LIST_HEAD(&mcd_dom->mcd_hash_list_head);
    mspin_unlock(&g_mcd_domain_list_lock);

    if ( domid > 0 ) 
        dom_weight_update();

    return -ERR_NO;

fail:
    mcd_domain_free(mcd_dom);
    return -ERR_XENFAULT;
}

EXPORT void mcd_destroy(void *p)
{
    struct domain *dom = (struct domain*)p;

    if( dom == NULL ) return;

    if ( !domain_is_dying(dom) ) {
        printk("mcd: mcd_destroy can only destroy dying client\n");
        return;
    }

    total_weight -= ((mcd_domain_t*)dom->mcd)->weight;
    mcd_domain_free((mcd_domain_t*)dom->mcd);

    dom_weight_update();
}

EXPORT void mcd_mem_upt_trap(struct domain *dom)
{
    mcd_domain_t *mcd_dom;

    if( dom == NULL ) return;

    mcd_dom = (mcd_domain_t*) dom->mcd;

    mspin_lock(&mcd_dom->lock);
    if( dom->max_pages == ~0U ) // dom0
        mcd_dom->nr_avail = 0;
    else
        mcd_dom->nr_avail = dom->max_pages - dom->tot_pages;
    mspin_unlock(&mcd_dom->lock);

    if( mcd_dom->nr_avail < 0 || mcd_dom->nr_avail > dom->max_pages ) {
        printk("mcd: mem mgmt is wrong, so disable to use it\n");
        mcd_dom->nr_avail = 0;
    }
}

/*
 * VM wants to use more memory, so determine whether we have to give back
 */
EXPORT void mcd_mem_inc_trap(struct domain *dom, unsigned int nr_pages) 
{
// TODO algorithm
/*
    mcd_domain_t *mcd_dom;

    if( dom == NULL ) return;
*/
}

/*
 * VM wants to surrender some memory. Do nothing
 */
EXPORT void mcd_mem_dec_trap(struct domain *dom, unsigned int nr_pages)
{
// TODO algorithm
/*
    mcd_domain_t *mcd_dom = (mcd_domain_t*) dom->mcd;

    if( dom == NULL ) return;
*/
}

/* 
 * Initial
 */
LOCAL int __init mcd_start(void)
{
    mcd_data.pri_nr_pages = 0;
    mcd_data.pri_nr_bytes = 0;
    mcd_data.shr_nr_pages = 0;
    mcd_data.shr_nr_bytes = 0;
    mcd_data.pri_nr_keys = 0;
    mcd_data.shr_nr_keys = 0;

    max_mem_bytes = 0;

    INIT_LIST_HEAD(&g_mcd_domain_list);

    #ifdef MCD_LINEAR_PROBE
    INIT_LIST_HEAD(&g_mcd_shared_hash_list);
    #elif defined(MCD_HASH_CHAINING)
    {
        int i;
        for(i = 0; i < MAX_LIST_HEAD; i++)
            INIT_LIST_HEAD(&g_mcd_shared_hash_list[i]);
    }
    #endif

    mspin_lock_init(&mcd_data.lock);
    mspin_lock_init(&g_mcd_shared_hash_lock);
    mspin_lock_init(&g_mcd_domain_list_lock);

    return -ERR_NO;
}

LOCAL int __init init_mcd(void)
{
    // only if tmem enabled
#if 0
    if ( !tmh_enabled() )
        return -ERR_NOTMEM;
#endif

    if ( !use_mcd() )
        return -ERR_NOMCD;

    if ( mcd_start() ) {
        printk("mcd: initialized\n"); 
        mcd_enabled_flag = 1;
    }
    else
        printk("mcd: initialization FAILED\n");

    // TODO ssd test
    rand_val(777);

    return -ERR_NO;
}
__initcall(init_mcd);

/*
 * Local variables:
 * mode: C
 * c-set-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
