/******************************************************************************
 * tmem.h
 *
 * Transcendent memory
 *
 * Copyright (c) 2008, Dan Magenheimer, Oracle Corp.
 */

#ifndef __XEN_TMEM_H__
#define __XEN_TMEM_H__

#ifdef __XEN__
#include <xen/tmem_xen.h>
#endif

#include <xen/rbtree.h>
#include <xen/radix-tree.h>
#include <xen/list.h>

extern void tmem_destroy(void *);
extern void *tmem_relinquish_pages(unsigned int, unsigned int);
extern unsigned long tmem_freeable_pages(void);

// jinho start : this is from tmem.c for sharing with mcd.c
#define EXPORT /* indicates code other modules are dependent upon */
#define FORWARD

#define TMEM_SPEC_VERSION 1

/************  INTERFACE TO TMEM HOST-DEPENDENT (tmh) CODE ************/

#define CLI_ID_NULL TMH_CLI_ID_NULL
#define cli_id_str  tmh_cli_id_str
#define client_str  tmh_client_str

/************ DEBUG and STATISTICS (+ some compression testing) *******/

#ifndef NDEBUG
#define SENTINELS
#define NOINLINE noinline
#else
#define NOINLINE
#endif

#ifdef SENTINELS
#define DECL_SENTINEL unsigned long sentinel;
#define SET_SENTINEL(_x,_y) _x->sentinel = _y##_SENTINEL
#define INVERT_SENTINEL(_x,_y) _x->sentinel = ~_y##_SENTINEL
#define ASSERT_SENTINEL(_x,_y) \
    ASSERT(_x->sentinel != ~_y##_SENTINEL);ASSERT(_x->sentinel == _y##_SENTINEL)
#ifdef __i386__
#define POOL_SENTINEL 0x87658765
#define OBJ_SENTINEL 0x12345678
#define OBJNODE_SENTINEL 0xfedcba09
#define PGD_SENTINEL  0x43214321
#else
#define POOL_SENTINEL 0x8765876587658765
#define OBJ_SENTINEL 0x1234567812345678
#define OBJNODE_SENTINEL 0xfedcba0987654321
#define PGD_SENTINEL  0x4321432143214321
#endif
#else
#define DECL_SENTINEL
#define SET_SENTINEL(_x,_y) do { } while (0)
#define ASSERT_SENTINEL(_x,_y) do { } while (0)
#define INVERT_SENTINEL(_x,_y) do { } while (0)
#endif

/************ CORE DATA STRUCTURES ************************************/

#define MAX_POOLS_PER_DOMAIN 16
#define MAX_GLOBAL_SHARED_POOLS  16

struct tm_pool;
struct tmem_page_descriptor;
struct tmem_page_content_descriptor;
struct client {
    struct list_head client_list;
    struct tm_pool *pools[MAX_POOLS_PER_DOMAIN];
    tmh_client_t *tmh;
    struct list_head ephemeral_page_list;
    long eph_count, eph_count_max;
    cli_id_t cli_id;
    uint32_t weight;
    uint32_t cap;
    bool_t compress;
    bool_t frozen;
    bool_t shared_auth_required;
    /* for save/restore/migration */
    bool_t live_migrating;
    bool_t was_frozen;
    struct list_head persistent_invalidated_list;
    struct tmem_page_descriptor *cur_pgp;
    /* statistics collection */
    unsigned long compress_poor, compress_nomem;
    unsigned long compressed_pages;
    uint64_t compressed_sum_size;
    uint64_t total_cycles;
    unsigned long succ_pers_puts, succ_eph_gets, succ_pers_gets;
    /* shared pool authentication */
    uint64_t shared_auth_uuid[MAX_GLOBAL_SHARED_POOLS][2];
};
typedef struct client client_t;

struct share_list {
    struct list_head share_list;
    client_t *client;
};
typedef struct share_list sharelist_t;

#define OBJ_HASH_BUCKETS 256 /* must be power of two */
#define OBJ_HASH_BUCKETS_MASK (OBJ_HASH_BUCKETS-1)

struct tm_pool {
    bool_t shared;
    bool_t persistent;
    bool_t is_dying;
    int pageshift; /* 0 == 2**12 */
    struct list_head pool_list;
    client_t *client;
    uint64_t uuid[2]; /* 0 for private, non-zero for shared */
    uint32_t pool_id;
    rwlock_t pool_rwlock;
    struct rb_root obj_rb_root[OBJ_HASH_BUCKETS]; /* protected by pool_rwlock */
    struct list_head share_list; /* valid if shared */
    int shared_count; /* valid if shared */
    /* for save/restore/migration */
    struct list_head persistent_page_list;
    struct tmem_page_descriptor *cur_pgp;
    /* statistics collection */
    atomic_t pgp_count;
    int pgp_count_max;
    long obj_count;  /* atomicity depends on pool_rwlock held for write */
    long obj_count_max;  
    unsigned long objnode_count, objnode_count_max;
    uint64_t sum_life_cycles;
    uint64_t sum_evicted_cycles;
    unsigned long puts, good_puts, no_mem_puts;
    unsigned long dup_puts_flushed, dup_puts_replaced;
    unsigned long gets, found_gets;
    unsigned long flushs, flushs_found;
    unsigned long flush_objs, flush_objs_found;
    DECL_SENTINEL
};
typedef struct tm_pool pool_t;

#define is_persistent(_p)  (_p->persistent)
#define is_ephemeral(_p)   (!(_p->persistent))
#define is_shared(_p)      (_p->shared)
#define is_private(_p)     (!(_p->shared))

struct oid {
    uint64_t oid[3];
};
typedef struct oid OID;

struct tmem_object_root {
    DECL_SENTINEL
    OID oid;
    struct rb_node rb_tree_node; /* protected by pool->pool_rwlock */
    unsigned long objnode_count; /* atomicity depends on obj_spinlock */
    long pgp_count; /* atomicity depends on obj_spinlock */
    struct radix_tree_root tree_root; /* tree of pages within object */
    pool_t *pool;
    cli_id_t last_client;
    spinlock_t obj_spinlock;
    bool_t no_evict; /* if globally locked, pseudo-locks against eviction */
};
typedef struct tmem_object_root obj_t;

typedef struct radix_tree_node rtn_t;
struct tmem_object_node {
    obj_t *obj;
    DECL_SENTINEL
    rtn_t rtn;
};
typedef struct tmem_object_node objnode_t;

struct tmem_page_descriptor {
    union {
        struct list_head global_eph_pages;
        struct list_head client_inv_pages;
    };
    union {
        struct {
            union {
                struct list_head client_eph_pages;
                struct list_head pool_pers_pages;
            };
            obj_t *obj;
        } us;
        OID inv_oid;  /* used for invalid list only */
    };
    pagesize_t size; /* 0 == PAGE_SIZE (pfp), -1 == data invalid,
                    else compressed data (cdata) */
    uint32_t index;
    /* must hold pcd_tree_rwlocks[firstbyte] to use pcd pointer/siblings */
    uint16_t firstbyte; /* NON_SHAREABLE->pfp  otherwise->pcd */
    bool_t eviction_attempted;  /* CHANGE TO lifetimes? (settable) */
    struct list_head pcd_siblings;
    union {
        pfp_t *pfp;  /* page frame pointer */
        char *cdata; /* compressed data */
        struct tmem_page_content_descriptor *pcd; /* page dedup */
    };
    union {
        uint64_t timestamp;
        uint32_t pool_id;  /* used for invalid list only */
    };
    DECL_SENTINEL
};
typedef struct tmem_page_descriptor pgp_t;

#define PCD_TZE_MAX_SIZE (PAGE_SIZE - (PAGE_SIZE/64))

struct tmem_page_content_descriptor {
    union {
        pfp_t *pfp;  /* page frame pointer */
        char *cdata; /* if compression_enabled */
        char *tze; /* if !compression_enabled, trailing zeroes eliminated */
    };
    struct list_head pgp_list;
    struct rb_node pcd_rb_tree_node;
    uint32_t pgp_ref_count;
    pagesize_t size; /* if compression_enabled -> 0<size<PAGE_SIZE (*cdata)
                     * else if tze, 0<=size<PAGE_SIZE, rounded up to mult of 8
                     * else PAGE_SIZE -> *pfp */
};
typedef struct tmem_page_content_descriptor pcd_t;

/************ CONCURRENCY  ***********************************************/

#define tmem_spin_lock(_l)  do {if (!tmh_lock_all) spin_lock(_l);}while(0)
#define tmem_spin_unlock(_l)  do {if (!tmh_lock_all) spin_unlock(_l);}while(0)
#define tmem_read_lock(_l)  do {if (!tmh_lock_all) read_lock(_l);}while(0)
#define tmem_read_unlock(_l)  do {if (!tmh_lock_all) read_unlock(_l);}while(0)
#define tmem_write_lock(_l)  do {if (!tmh_lock_all) write_lock(_l);}while(0)
#define tmem_write_unlock(_l)  do {if (!tmh_lock_all) write_unlock(_l);}while(0)
#define tmem_write_trylock(_l)  ((tmh_lock_all)?1:write_trylock(_l))
#define tmem_spin_trylock(_l)  (tmh_lock_all?1:spin_trylock(_l))

#define ASSERT_SPINLOCK(_l) ASSERT(tmh_lock_all || spin_is_locked(_l))
#define ASSERT_WRITELOCK(_l) ASSERT(tmh_lock_all || rw_is_write_locked(_l))


#define atomic_inc_and_max(_c) do { \
    atomic_inc(&_c); \
    if ( _atomic_read(_c) > _c##_max ) \
        _c##_max = _atomic_read(_c); \
} while (0)

#define atomic_dec_and_assert(_c) do { \
    atomic_dec(&_c); \
    ASSERT(_atomic_read(_c) >= 0); \
} while (0)


/************ MEMORY ALLOCATION INTERFACE *****************************/

#define tmem_malloc(_type,_pool) \
       _tmem_malloc(sizeof(_type), __alignof__(_type), _pool)

#define tmem_malloc_bytes(_size,_pool) \
       _tmem_malloc(_size, 1, _pool)

#define MAX_EVICTS 10  /* should be variable or set via TMEMC_ ?? */
#define NOT_SHAREABLE ((uint16_t)-1UL)
#define BSIZE 1024
// jinho end

#endif /* __XEN_TMEM_H__ */
