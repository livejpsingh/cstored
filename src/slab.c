#include "kvs/slab.h"

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <sys/mman.h>
#  include <unistd.h>
#endif

/* Magic cookies for chunk integrity and double-free detection */
#define KVS_CHUNK_MAGIC_ALLOCATED (0xCAFEBABE74657374ULL)
#define KVS_CHUNK_MAGIC_FREE      (0xDEADBEEF534C4142ULL)
#define KVS_SLAB_MAGIC            (0x534C4142504F4F4CULL) /* "SLABPOOL" */

/*
 * Header prefixed to each chunk payload.
 * Invariant: Must be padded so payload starts at a 16-byte aligned address.
 */
typedef struct kvs_chunk_hdr {
    uint64_t magic;               /* Allocation validation cookie */
    struct kvs_slab *owning_slab; /* Direct pointer to parent slab */
    struct kvs_chunk_hdr *next;   /* Free list singly-linked pointer */
    uint64_t _padding;            /* Alignment padding to enforce 32-byte header size */
} kvs_chunk_hdr_t;

/* Single contiguous OS page allocation containing multiple chunks */
typedef struct kvs_slab {
    uint64_t magic;               /* Slab integrity identifier */
    struct kvs_slab *next;        /* Next slab in the pool's slab chain */
    size_t mapped_size;           /* Exact byte count passed to mmap/VirtualAlloc */
    size_t free_count;            /* Number of free chunks currently in this slab */
    uint8_t *payload_start;       /* Base address of chunk array */
    uint8_t *payload_end;         /* Exclusive upper address bound of chunk array */
} kvs_slab_t;

/* Primary pool controller */
struct kvs_slab_pool {
    uint64_t magic;
    size_t user_chunk_size;       /* Requested chunk size */
    size_t aligned_chunk_size;    /* User chunk size rounded up to 16 bytes */
    size_t chunk_stride;          /* sizeof(kvs_chunk_hdr_t) + aligned_chunk_size */
    size_t chunks_per_slab;       /* Chunks allocated per slab page */
    size_t max_slabs;             /* Maximum allowed slabs (0 = unlimited) */
    size_t current_slabs;         /* Number of active mapped slabs */
    size_t active_allocations;    /* Total currently allocated chunks */
    size_t total_bytes_mapped;    /* Total memory mapped from operating system */
    kvs_chunk_hdr_t *free_list;   /* Head of global free list for O(1) pop/push */
    kvs_slab_t *slabs_head;       /* Head of mapped slabs list for destruction */
};

/*
 * Operating system page mapping abstraction.
 * Allocates anonymous, non-executable read/write pages.
 */
static void *kvs_os_page_alloc(size_t size) {
#if defined(_WIN32)
    return VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
    void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) {
        return NULL;
    }
    return ptr;
#endif
}

/*
 * Operating system page unmapping abstraction.
 */
static void kvs_os_page_free(void *ptr, size_t size) {
    if (ptr == NULL || size == 0) {
        return;
    }
#if defined(_WIN32)
    (void)size;
    VirtualFree(ptr, 0, MEM_RELEASE);
#else
    munmap(ptr, size);
#endif
}

/*
 * Retrieves system page size for page-aligned allocations.
 */
static size_t kvs_get_system_page_size(void) {
#if defined(_WIN32)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (size_t)si.dwPageSize;
#else
    long sz = sysconf(_SC_PAGESIZE);
    if (sz <= 0) {
        return 4096U;
    }
    return (size_t)sz;
#endif
}

/*
 * Allocates and initializes a new slab page.
 * Avoids TOCTOU and integer overflow during slab layout calculation.
 */
static kvs_status_t kvs_slab_create_new(kvs_slab_pool_t *pool, kvs_slab_t **out_slab) {
    if (pool == NULL || out_slab == NULL) {
        return KVS_ERR_INVALID_ARG;
    }

    if (pool->max_slabs > 0 && pool->current_slabs >= pool->max_slabs) {
        return KVS_ERR_NOMEM;
    }

    /* Calculate header alignment and raw bytes needed */
    const size_t slab_hdr_size = kvs_align_up(sizeof(kvs_slab_t), KVS_DEFAULT_ALIGNMENT);
    size_t chunks_payload_bytes = 0;
    if (kvs_mul_overflow_size(pool->chunks_per_slab, pool->chunk_stride, &chunks_payload_bytes)) {
        return KVS_ERR_OVERFLOW;
    }

    size_t total_needed = 0;
    if (kvs_add_overflow_size(slab_hdr_size, chunks_payload_bytes, &total_needed)) {
        return KVS_ERR_OVERFLOW;
    }

    /* Round up to system page boundary to ensure clean page mapping */
    const size_t page_sz = kvs_get_system_page_size();
    const size_t mapped_size = kvs_align_up(total_needed, page_sz);
    if (mapped_size == 0) {
        return KVS_ERR_OVERFLOW;
    }

    uint8_t *raw_mem = (uint8_t *)kvs_os_page_alloc(mapped_size);
    if (raw_mem == NULL) {
        return KVS_ERR_NOMEM;
    }

    kvs_slab_t *slab = (kvs_slab_t *)(void *)raw_mem;
    slab->magic = KVS_SLAB_MAGIC;
    slab->mapped_size = mapped_size;
    slab->free_count = pool->chunks_per_slab;
    slab->payload_start = raw_mem + slab_hdr_size;
    slab->payload_end = slab->payload_start + chunks_payload_bytes;
    slab->next = pool->slabs_head;
    pool->slabs_head = slab;

    /* Populate chunk headers and link into the pool's free list */
    for (size_t i = 0; i < pool->chunks_per_slab; i++) {
        uint8_t *chunk_addr = slab->payload_start + (i * pool->chunk_stride);
        kvs_chunk_hdr_t *hdr = (kvs_chunk_hdr_t *)(void *)chunk_addr;
        hdr->magic = KVS_CHUNK_MAGIC_FREE;
        hdr->owning_slab = slab;
        hdr->next = pool->free_list;
        hdr->_padding = 0;
        pool->free_list = hdr;
    }

    pool->current_slabs++;
    pool->total_bytes_mapped += mapped_size;

    *out_slab = slab;
    return KVS_OK;
}

kvs_status_t kvs_slab_pool_create(
    size_t chunk_size,
    size_t chunks_per_slab,
    size_t max_slabs,
    kvs_slab_pool_t **out_pool
) {
    if (out_pool == NULL || chunk_size == 0 || chunks_per_slab == 0) {
        return KVS_ERR_INVALID_ARG;
    }

    /*
     * Enforce minimum chunk payload of 16 bytes for pointer storage when free.
     * Enforce maximum chunk size to avoid pathological slab sizes.
     */
    if (chunk_size > (1024U * 1024U * 16U)) { /* 16MB max slab chunk */
        return KVS_ERR_INVALID_ARG;
    }

    kvs_slab_pool_t *pool = (kvs_slab_pool_t *)malloc(sizeof(kvs_slab_pool_t));
    if (pool == NULL) {
        return KVS_ERR_NOMEM;
    }

    memset(pool, 0, sizeof(*pool));
    pool->magic = KVS_SLAB_MAGIC;
    pool->user_chunk_size = chunk_size;
    pool->aligned_chunk_size = kvs_align_up(chunk_size, KVS_DEFAULT_ALIGNMENT);
    
    /* Ensure chunk header itself is 16-byte aligned */
    const size_t hdr_size = kvs_align_up(sizeof(kvs_chunk_hdr_t), KVS_DEFAULT_ALIGNMENT);
    if (kvs_add_overflow_size(hdr_size, pool->aligned_chunk_size, &pool->chunk_stride)) {
        free(pool);
        return KVS_ERR_OVERFLOW;
    }

    pool->chunks_per_slab = chunks_per_slab;
    pool->max_slabs = max_slabs;
    pool->current_slabs = 0;
    pool->active_allocations = 0;
    pool->total_bytes_mapped = 0;
    pool->free_list = NULL;
    pool->slabs_head = NULL;

    /* Pre-allocate the initial slab */
    kvs_slab_t *first_slab = NULL;
    kvs_status_t status = kvs_slab_create_new(pool, &first_slab);
    if (status != KVS_OK) {
        free(pool);
        return status;
    }

    *out_pool = pool;
    return KVS_OK;
}

void *kvs_slab_alloc(kvs_slab_pool_t *pool) {
    if (pool == NULL || pool->magic != KVS_SLAB_MAGIC) {
        return NULL;
    }

    /* If free list is empty, attempt to map a new slab */
    if (pool->free_list == NULL) {
        kvs_slab_t *new_slab = NULL;
        kvs_status_t status = kvs_slab_create_new(pool, &new_slab);
        if (status != KVS_OK) {
            return NULL; /* Out of memory / hit max_slabs limit */
        }
    }

    /* Pop head from free list in O(1) */
    kvs_chunk_hdr_t *hdr = pool->free_list;
    if (hdr == NULL) {
        return NULL;
    }

    /* Invariant: chunk popped must have had FREE magic */
    if (hdr->magic != KVS_CHUNK_MAGIC_FREE) {
        return NULL; /* Corruption detected */
    }

    pool->free_list = hdr->next;
    hdr->next = NULL;
    hdr->magic = KVS_CHUNK_MAGIC_ALLOCATED;

    if (hdr->owning_slab != NULL) {
        hdr->owning_slab->free_count--;
    }

    pool->active_allocations++;

    /* Payload begins immediately after aligned header */
    const size_t hdr_offset = kvs_align_up(sizeof(kvs_chunk_hdr_t), KVS_DEFAULT_ALIGNMENT);
    uint8_t *payload = ((uint8_t *)(void *)hdr) + hdr_offset;

    return (void *)payload;
}

kvs_status_t kvs_slab_free(kvs_slab_pool_t *pool, void *ptr) {
    if (pool == NULL || pool->magic != KVS_SLAB_MAGIC || ptr == NULL) {
        return KVS_ERR_INVALID_ARG;
    }

    /* Verify payload pointer alignment */
    if (((uintptr_t)ptr % KVS_DEFAULT_ALIGNMENT) != 0) {
        return KVS_ERR_INVALID_ARG;
    }

    const size_t hdr_offset = kvs_align_up(sizeof(kvs_chunk_hdr_t), KVS_DEFAULT_ALIGNMENT);
    const uint8_t *raw_ptr = ((const uint8_t *)ptr) - hdr_offset;

    /* Verify pointer belongs to one of this pool's slabs before any dereference */
    kvs_slab_t *slab = pool->slabs_head;
    bool valid_slab = false;
    while (slab != NULL) {
        if (slab->magic == KVS_SLAB_MAGIC &&
            raw_ptr >= slab->payload_start &&
            raw_ptr < slab->payload_end) {
            size_t offset = (size_t)(raw_ptr - slab->payload_start);
            if ((offset % pool->chunk_stride) == 0) {
                valid_slab = true;
                break;
            }
        }
        slab = slab->next;
    }

    if (!valid_slab) {
        return KVS_ERR_INVALID_ARG; /* Foreign pointer (e.g. stack, malloc heap, or out of range) */
    }

    kvs_chunk_hdr_t *hdr = (kvs_chunk_hdr_t *)(void *)raw_ptr;

    /* Double-free detection pitfall avoided */
    if (hdr->magic == KVS_CHUNK_MAGIC_FREE) {
        return KVS_ERR_DOUBLE_FREE;
    }

    if (hdr->magic != KVS_CHUNK_MAGIC_ALLOCATED) {
        return KVS_ERR_INVALID_ARG; /* Corrupted or foreign pointer */
    }

    if (hdr->owning_slab != slab) {
        return KVS_ERR_INVALID_ARG;
    }

    /* Transition state to FREE and re-link into pool's free list */
    hdr->magic = KVS_CHUNK_MAGIC_FREE;
    hdr->next = pool->free_list;
    pool->free_list = hdr;

    slab->free_count++;
    if (pool->active_allocations > 0) {
        pool->active_allocations--;
    }

    return KVS_OK;
}

kvs_status_t kvs_slab_pool_stats(const kvs_slab_pool_t *pool, kvs_slab_stats_t *out_stats) {
    if (pool == NULL || pool->magic != KVS_SLAB_MAGIC || out_stats == NULL) {
        return KVS_ERR_INVALID_ARG;
    }

    out_stats->chunk_size = pool->user_chunk_size;
    out_stats->chunks_per_slab = pool->chunks_per_slab;
    out_stats->total_slabs = pool->current_slabs;
    out_stats->total_chunks = pool->current_slabs * pool->chunks_per_slab;
    out_stats->active_allocations = pool->active_allocations;
    out_stats->total_bytes_mapped = pool->total_bytes_mapped;

    return KVS_OK;
}

void kvs_slab_pool_destroy(kvs_slab_pool_t *pool) {
    if (pool == NULL) {
        return;
    }

    if (pool->magic != KVS_SLAB_MAGIC) {
        return;
    }

    /* Unmap all OS pages cleanly */
    kvs_slab_t *current = pool->slabs_head;
    while (current != NULL) {
        kvs_slab_t *next = current->next;
        const size_t sz = current->mapped_size;
        current->magic = 0; /* Invalidate magic before unmap */
        kvs_os_page_free(current, sz);
        current = next;
    }

    pool->magic = 0;
    free(pool);
}
