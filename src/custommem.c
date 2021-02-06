#define _GNU_SOURCE         /* See feature_test_macros(7) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <signal.h>

#include "box86context.h"
#include "elfloader.h"
#include "debug.h"
#include "x86trace.h"
#include "x86emu.h"
#include "librarian.h"
#include "bridge.h"
#include "library.h"
#include "callback.h"
#include "wrapper.h"
#include "myfts.h"
#include "threads.h"
#include "x86trace.h"
#include "signals.h"
#include <sys/mman.h>
#include "custommem.h"
#ifdef DYNAREC
#include "dynablock.h"
#include "dynarec/arm_lock_helper.h"
#include "khash.h"


// init inside dynablocks.c
KHASH_MAP_INIT_INT(dynablocks, dynablock_t*)
static dynablocklist_t**   dynmap;     // 4G of memory mapped by 4K block
static pthread_mutex_t     mutex_mmap;
static mmaplist_t          *mmaplist;
static int                 mmapsize;
static kh_dynablocks_t     *dblist_oversized;      // store the list of oversized dynablocks (normal sized are inside mmaplist)
static uintptr_t           **box86_jumptable = NULL;
static uintptr_t           *box86_jmptbl_default = NULL;
#endif
static uint32_t*           memprot;    // protection flags by 4K block

typedef struct blocklist_s {
    void*               block;
    int                 maxfree;
    size_t              size;
} blocklist_t;

#define MMAPSIZE (256*1024)      // allocate 256kb sized blocks

static pthread_mutex_t     mutex_blocks = PTHREAD_MUTEX_INITIALIZER;
static int                 n_blocks = 0;       // number of blocks for custom malloc
static blocklist_t*        p_blocks = NULL;    // actual blocks for custom malloc

typedef union mark_s {
    struct {
        unsigned int    fill:1;
        unsigned int    size:31;
    };
    uint32_t            x32;
} mark_t;
typedef struct blockmark_s {
    mark_t  prev;
    mark_t  next;
} blockmark_t;


// get first subblock free in block, stating at start (from block). return NULL if no block, else first subblock free (mark included), filling size
static void* getFirstBlock(void* block, int maxsize, int* size)
{
    // get start of block
    blockmark_t *m = (blockmark_t*)block;
    while(m->next.x32) {    // while there is a subblock
        if(!m->next.fill && m->next.size>=maxsize+sizeof(blockmark_t)) {
            *size = m->next.size;
            return m;
        }
        m = (blockmark_t*)((uintptr_t)m + m->next.size);
    }

    return NULL;
}

static int getMaxFreeBlock(void* block, size_t block_size)
{
    // get start of block
    blockmark_t *m = (blockmark_t*)((uintptr_t)block+block_size-sizeof(blockmark_t)); // styart with the end
    int maxsize = 0;
    while(m->prev.x32) {    // while there is a subblock
        if(!m->prev.fill && m->prev.size>maxsize) {
            maxsize = m->prev.size;
            if((uintptr_t)block+maxsize>(uintptr_t)m)
                return maxsize; // no block large enough left...
        }
        m = (blockmark_t*)((uintptr_t)m - m->prev.size);
    }
    return maxsize;
}

static void* allocBlock(void* block, void *sub, int size)
{
    blockmark_t *s = (blockmark_t*)sub;
    blockmark_t *n = (blockmark_t*)((uintptr_t)s + s->next.size);

    s->next.fill = 1;
    s->next.size = size+sizeof(blockmark_t);
    blockmark_t *m = (blockmark_t*)((uintptr_t)s + s->next.size);   // this is new n
    m->prev.fill = 1;
    m->prev.size = s->next.size;
    if(n!=m) {
        // new mark
        m->prev.fill = 1;
        m->prev.size = s->next.size;
        m->next.fill = 0;
        m->next.size = (uintptr_t)n - (uintptr_t)m;
        n->prev.fill = 0;
        n->prev.size = m->next.size;
    }

    return (void*)((uintptr_t)sub + sizeof(blockmark_t));
}
static void freeBlock(void *block, void* sub)
{
    blockmark_t *m = (blockmark_t*)block;
    blockmark_t *s = (blockmark_t*)sub;
    blockmark_t *n = (blockmark_t*)((uintptr_t)s + s->next.size);
    if(block!=sub)
        m = (blockmark_t*)((uintptr_t)s - s->prev.size);
    s->next.fill = 0;
    n->prev.fill = 0;
    // check if merge with previous
    if (s->prev.x32 && !s->prev.fill) {
        // remove s...
        m->next.size += s->next.size;
        n->prev.size = m->next.size;
        s = m;
    }
    // check if merge with next
    if(n->next.x32 && !n->next.fill) {
        blockmark_t *n2 = (blockmark_t*)((uintptr_t)n + n->next.size);
        //remove n
        s->next.size += n->next.size;
        n2->prev.size = s->next.size;
    }
}
// return 1 if block has been expanded to new size, 0 if not
static int expandBlock(void* block, void* sub, int newsize)
{
    newsize = (newsize+3)&~3;
    blockmark_t *s = (blockmark_t*)sub;
    blockmark_t *n = (blockmark_t*)((uintptr_t)s + s->next.size);
    if(s->next.fill)
        return 0;   // next block is filled
    if(s->next.size + n->next.size < newsize)
        return 0;   // free space too short
    // ok, doing the alloc!
    s->next.size = newsize+sizeof(blockmark_t);
    blockmark_t *m = (blockmark_t*)((uintptr_t)s + s->next.size);   // this is new n
    m->prev.fill = 1;
    m->prev.size = s->next.size;
    if(n!=m) {
        // new mark
        m->prev.fill = 1;
        m->prev.size = s->next.size;
        m->next.fill = 0;
        m->next.size = (uintptr_t)n - (uintptr_t)m;
        n->prev.fill = 0;
        n->prev.size = m->next.size;
    }
    return 1;
}
// return size of block
static int sizeBlock(void* sub)
{
    blockmark_t *s = (blockmark_t*)sub;
    return s->next.size;
}

void* customMalloc(size_t size)
{
    // look for free space
    void* sub = NULL;
    pthread_mutex_lock(&mutex_blocks);
    for(int i=0; i<n_blocks; ++i) {
        if(p_blocks[i].maxfree>=size) {
            int rsize = 0;
            sub = getFirstBlock(p_blocks[i].block, size, &rsize);
            if(sub) {
                void* ret = allocBlock(p_blocks[i].block, sub, size);
                if(rsize==p_blocks[i].maxfree)
                    p_blocks[i].maxfree = getMaxFreeBlock(p_blocks[i].block, p_blocks[i].size);
                pthread_mutex_unlock(&mutex_blocks);
                return ret;
            }
        }
    }
    // add a new block
    int i = n_blocks++;
    p_blocks = (blocklist_t*)realloc(p_blocks, n_blocks*sizeof(blocklist_t));
    size_t allocsize = MMAPSIZE;
    if(size+2*sizeof(blockmark_t)>allocsize)
        allocsize = size+2*sizeof(blockmark_t);
    void* p = calloc(1, allocsize);
    p_blocks[i].block = p;
    p_blocks[i].size = allocsize;
    // setup marks
    blockmark_t* m = (blockmark_t*)p;
    m->prev.x32 = 0;
    m->next.fill = 0;
    m->next.size = allocsize-sizeof(blockmark_t);
    m = (blockmark_t*)(p+allocsize-sizeof(blockmark_t));
    m->next.x32 = 0;
    m->prev.fill = 0;
    m->prev.size = allocsize-sizeof(blockmark_t);
    // alloc 1st block
    void* ret  = allocBlock(p_blocks[i].block, p, size);
    p_blocks[i].maxfree = getMaxFreeBlock(p_blocks[i].block, p_blocks[i].size);
    pthread_mutex_unlock(&mutex_blocks);
    return ret;
}
void* customCalloc(size_t n, size_t size)
{
    size_t newsize = n*size;
    void* ret = customMalloc(newsize);
    memset(ret, 0, newsize);
    return ret;
}
void* customRealloc(void* p, size_t size)
{
    if(!p)
        return customMalloc(size);
    uintptr_t addr = (uintptr_t)p;
    pthread_mutex_lock(&mutex_blocks);
    for(int i=0; i<n_blocks; ++i) {
        if ((addr>(uintptr_t)p_blocks[i].block) 
         && (addr<((uintptr_t)p_blocks[i].block+p_blocks[i].size))) {
            void* sub = (void*)(addr-sizeof(blockmark_t));
            if(expandBlock(p_blocks[i].block, sub, size)) {
                p_blocks[i].maxfree = getMaxFreeBlock(p_blocks[i].block, p_blocks[i].size);
                pthread_mutex_unlock(&mutex_blocks);
                return p;
            }
                pthread_mutex_unlock(&mutex_blocks);
                void* newp = customMalloc(size);
                memcpy(newp, p, sizeBlock(sub));
                customFree(p);
                return newp;
            
        }
    }
    pthread_mutex_unlock(&mutex_blocks);
    if(n_blocks)
        dynarec_log(LOG_NONE, "Warning, block %p not found in p_blocks for Realloc, Malloc'ng again without free\n", (void*)addr);
    return customMalloc(size);
}
void customFree(void* p)
{
    if(!p)
        return;
    uintptr_t addr = (uintptr_t)p;
    pthread_mutex_lock(&mutex_blocks);
    for(int i=0; i<n_blocks; ++i) {
        if ((addr>(uintptr_t)p_blocks[i].block) 
         && (addr<((uintptr_t)p_blocks[i].block+p_blocks[i].size))) {
            void* sub = (void*)(addr-sizeof(blockmark_t));
            freeBlock(p_blocks[i].block, sub);
            p_blocks[i].maxfree = getMaxFreeBlock(p_blocks[i].block, p_blocks[i].size);
            pthread_mutex_unlock(&mutex_blocks);
            return;
        }
    }
    pthread_mutex_unlock(&mutex_blocks);
    if(n_blocks)
        dynarec_log(LOG_NONE, "Warning, block %p not found in p_blocks for Free\n", (void*)addr);
}

#ifdef DYNAREC
typedef struct mmaplist_s {
    void*               block;
    int                 maxfree;
    size_t              size;
    kh_dynablocks_t*    dblist;
} mmaplist_t;

uintptr_t FindFreeDynarecMap(dynablock_t* db, int size)
{
    // look for free space
    void* sub = NULL;
    for(int i=0; i<mmapsize; ++i) {
        if(mmaplist[i].maxfree>=size) {
            int rsize = 0;
            sub = getFirstBlock(mmaplist[i].block, size, &rsize);
            if(sub) {
                uintptr_t ret = (uintptr_t)allocBlock(mmaplist[i].block, sub, size);
                if(rsize==mmaplist[i].maxfree)
                    mmaplist[i].maxfree = getMaxFreeBlock(mmaplist[i].block, mmaplist[i].size);
                kh_dynablocks_t *blocks = mmaplist[i].dblist;
                if(!blocks) {
                    blocks = mmaplist[i].dblist = kh_init(dynablocks);
                    kh_resize(dynablocks, blocks, 64);
                }
                khint_t k;
                int r;
                k = kh_put(dynablocks, blocks, (uintptr_t)db, &r);
                kh_value(blocks, k) = db;
                return ret;
            }
        }
    }
    return 0;
}

uintptr_t AddNewDynarecMap(dynablock_t* db, int size)
{
    int i = mmapsize++;    // yeah, useful post incrementation
    dynarec_log(LOG_DEBUG, "Ask for DynaRec Block Alloc #%d\n", mmapsize);
    mmaplist = (mmaplist_t*)realloc(mmaplist, mmapsize*sizeof(mmaplist_t));
    void *p = NULL;
    if(posix_memalign(&p, box86_pagesize, MMAPSIZE)) {
        dynarec_log(LOG_INFO, "Cannot create memory map of %d byte for dynarec block #%d\n", MMAPSIZE, i);
        --mmapsize;
        return 0;
    }
    mprotect(p, MMAPSIZE, PROT_READ | PROT_WRITE | PROT_EXEC);
    setProtection((uintptr_t)p, MMAPSIZE, PROT_READ | PROT_WRITE | PROT_EXEC);

    mmaplist[i].block = p;
    mmaplist[i].size = MMAPSIZE;
    // setup marks
    blockmark_t* m = (blockmark_t*)p;
    m->prev.x32 = 0;
    m->next.fill = 0;
    m->next.size = MMAPSIZE-sizeof(blockmark_t);
    m = (blockmark_t*)(p+MMAPSIZE-sizeof(blockmark_t));
    m->next.x32 = 0;
    m->prev.fill = 0;
    m->prev.size = MMAPSIZE-sizeof(blockmark_t);
    // alloc 1st block
    uintptr_t sub  = (uintptr_t)allocBlock(mmaplist[i].block, p, size);
    mmaplist[i].maxfree = getMaxFreeBlock(mmaplist[i].block, mmaplist[i].size);
    kh_dynablocks_t *blocks = mmaplist[i].dblist = kh_init(dynablocks);
    kh_resize(dynablocks, blocks, 64);
    khint_t k;
    int ret;
    k = kh_put(dynablocks, blocks, (uintptr_t)db, &ret);
    kh_value(blocks, k) = db;
    return sub;
}

void ActuallyFreeDynarecMap(dynablock_t* db, uintptr_t addr, int size)
{
    if(!addr || !size)
        return;
    for(int i=0; i<mmapsize; ++i) {
        if ((addr>(uintptr_t)mmaplist[i].block) 
         && (addr<((uintptr_t)mmaplist[i].block+mmaplist[i].size))) {
            void* sub = (void*)(addr-sizeof(blockmark_t));
            freeBlock(mmaplist[i].block, sub);
            mmaplist[i].maxfree = getMaxFreeBlock(mmaplist[i].block, mmaplist[i].size);
            kh_dynablocks_t *blocks = mmaplist[i].dblist;
            if(blocks) {
                khint_t k = kh_get(dynablocks, blocks, (uintptr_t)db);
                if(k!=kh_end(blocks))
                    kh_del(dynablocks, blocks, k);
            }
            return;
        }
    }
    if(mmapsize)
        dynarec_log(LOG_NONE, "Warning, block %p (size %d) not found in mmaplist for Free\n", (void*)addr, size);
}

dynablock_t* FindDynablockFromNativeAddress(void* addr)
{
    // look in actual list
    for(int i=0; i<mmapsize; ++i) {
        if ((uintptr_t)addr>=(uintptr_t)mmaplist[i].block 
        && ((uintptr_t)addr<(uintptr_t)mmaplist[i].block+mmaplist[i].size))
            return FindDynablockDynablocklist(addr, mmaplist[i].dblist);
    }
    // look in oversized
    return FindDynablockDynablocklist(addr, dblist_oversized);
}

uintptr_t AllocDynarecMap(dynablock_t* db, int size)
{
    if(!size)
        return 0;
    if(size>MMAPSIZE-2*sizeof(blockmark_t)) {
        void *p = NULL;
        if(posix_memalign(&p, box86_pagesize, size)) {
            dynarec_log(LOG_INFO, "Cannot create dynamic map of %d bytes\n", size);
            return 0;
        }
        mprotect(p, size, PROT_READ | PROT_WRITE | PROT_EXEC);
        setProtection((uintptr_t)p, size, PROT_READ | PROT_WRITE | PROT_EXEC);
        kh_dynablocks_t *blocks = dblist_oversized;
        if(!blocks) {
            blocks = dblist_oversized = kh_init(dynablocks);
            kh_resize(dynablocks, blocks, 64);
        }
        khint_t k;
        int ret;
        k = kh_put(dynablocks, blocks, (uintptr_t)db, &ret);
        kh_value(blocks, k) = db;
        return (uintptr_t)p;
    }
    
    if(pthread_mutex_trylock(&mutex_mmap)) {
        sched_yield();  // give it a chance
        if(pthread_mutex_trylock(&mutex_mmap))
            return 0;   // cannot lock, baillout
    }

    uintptr_t ret = FindFreeDynarecMap(db, size);
    if(!ret)
        ret = AddNewDynarecMap(db, size);

    pthread_mutex_unlock(&mutex_mmap);

    return ret;
}

void FreeDynarecMap(dynablock_t* db, uintptr_t addr, uint32_t size)
{
    if(size>MMAPSIZE-2*sizeof(blockmark_t)) {
        free((void*)addr);
        kh_dynablocks_t *blocks = dblist_oversized;
        if(blocks) {
            khint_t k = kh_get(dynablocks, blocks, (uintptr_t)db);
            if(k!=kh_end(blocks))
                kh_del(dynablocks, blocks, k);
        }
        return;
    }
    pthread_mutex_lock(&mutex_mmap);
    ActuallyFreeDynarecMap(db, addr, size);
    pthread_mutex_unlock(&mutex_mmap);
}

dynablocklist_t* getDB(uintptr_t idx)
{
    return dynmap[idx];
}

// each dynmap is 64k of size

void addDBFromAddressRange(uintptr_t addr, uintptr_t size)
{
    dynarec_log(LOG_DEBUG, "addDBFromAddressRange %p -> %p\n", (void*)addr, (void*)(addr+size-1));
    uintptr_t idx = (addr>>DYNAMAP_SHIFT);
    uintptr_t end = ((addr+size-1)>>DYNAMAP_SHIFT);
    for (uintptr_t i=idx; i<=end; ++i) {
        if(!dynmap[i]) {
            dynmap[i] = NewDynablockList(i<<DYNAMAP_SHIFT, 1<<DYNAMAP_SHIFT, 0);
        }
    }
}

void cleanDBFromAddressRange(uintptr_t addr, uintptr_t size, int destroy)
{
    dynarec_log(LOG_DEBUG, "cleanDBFromAddressRange %p -> %p %s\n", (void*)addr, (void*)(addr+size-1), destroy?"destroy":"mark");
    uintptr_t idx = (addr>box86_dynarec_largest && !destroy)?((addr-box86_dynarec_largest)>>DYNAMAP_SHIFT):(addr>>DYNAMAP_SHIFT);
    uintptr_t end = ((addr+size-1)>>DYNAMAP_SHIFT);
    for (uintptr_t i=idx; i<=end; ++i) {
        dynablocklist_t* dblist = dynmap[i];
        if(dblist) {
            uintptr_t startdb = StartDynablockList(dblist);
            uintptr_t enddb = EndDynablockList(dblist);
            uintptr_t startaddr = addr;
            if(startaddr<startdb) startaddr = startdb;
            uintptr_t endaddr = addr + size - 1;
            if(endaddr>enddb) endaddr = enddb;
            if(destroy)
                FreeRangeDynablock(dblist, startaddr, endaddr-startaddr+1);
            else
                MarkRangeDynablock(dblist, startaddr, endaddr-startaddr+1);
        }
    }
}

#ifdef ARM
void arm_next(void);
#endif

void addJumpTableIfDefault(void* addr, void* jmp)
{
    const uintptr_t idx = ((uintptr_t)addr>>DYNAMAP_SHIFT);
    if(box86_jumptable[idx] == box86_jmptbl_default) {
        uintptr_t* tbl = (uintptr_t*)malloc((1<<DYNAMAP_SHIFT)*sizeof(uintptr_t));
        for(int i=0; i<(1<<DYNAMAP_SHIFT); ++i)
            tbl[i] = (uintptr_t)arm_next;
        box86_jumptable[idx] = tbl;
    }
    const uintptr_t off = (uintptr_t)addr&((1<<DYNAMAP_SHIFT)-1);
    if(box86_jumptable[idx][off]==(uintptr_t)arm_next)
        box86_jumptable[idx][off] = (uintptr_t)jmp;
}
void setJumpTableDefault(void* addr)
{
    const uintptr_t idx = ((uintptr_t)addr>>DYNAMAP_SHIFT);
    if(box86_jumptable[idx] == box86_jmptbl_default) {
        return;
    }
    const uintptr_t off = (uintptr_t)addr&((1<<DYNAMAP_SHIFT)-1);
    box86_jumptable[idx][off] = (uintptr_t)arm_next;
}
uintptr_t getJumpTable()
{
    return (uintptr_t)box86_jumptable;
}

uintptr_t getJumpTableAddress(uintptr_t addr)
{
    const uintptr_t idx = ((uintptr_t)addr>>DYNAMAP_SHIFT);
    if(box86_jumptable[idx] == box86_jmptbl_default) {
        uintptr_t* tbl = (uintptr_t*)malloc((1<<DYNAMAP_SHIFT)*sizeof(uintptr_t));
        for(int i=0; i<(1<<DYNAMAP_SHIFT); ++i)
            tbl[i] = (uintptr_t)arm_next;
        box86_jumptable[idx] = tbl;
    }
    const uintptr_t off = (uintptr_t)addr&((1<<DYNAMAP_SHIFT)-1);
    return (uintptr_t)&box86_jumptable[idx][off];
}

// Remove the Write flag from an adress range, so DB can be executed
// no log, as it can be executed inside a signal handler
void protectDB(uintptr_t addr, uintptr_t size)
{
    dynarec_log(LOG_DEBUG, "protectDB %p -> %p\n", (void*)addr, (void*)(addr+size-1));
    uintptr_t idx = (addr>>DYNAMAP_SHIFT);
    uintptr_t end = ((addr+size-1)>>DYNAMAP_SHIFT);
    for (uintptr_t i=idx; i<=end; ++i) {
        uint32_t prot;
        do {
            prot=arm_lock_read_d(&memprot[i]);
            if(!prot)
                prot = PROT_READ | PROT_WRITE;    // comes from malloc & co, so should not be able to execute
        } while(arm_lock_write_d(&memprot[i], prot|PROT_DYNAREC));
        if(!(prot&PROT_DYNAREC))
            mprotect((void*)(i<<DYNAMAP_SHIFT), 1<<DYNAMAP_SHIFT, prot&~PROT_WRITE);
    }
}

// Add the Write flag from an adress range, and mark all block as dirty
// no log, as it can be executed inside a signal handler
void unprotectDB(uintptr_t addr, uintptr_t size)
{
    dynarec_log(LOG_DEBUG, "unprotectDB %p -> %p\n", (void*)addr, (void*)(addr+size-1));
    uintptr_t idx = (addr>>DYNAMAP_SHIFT);
    uintptr_t end = ((addr+size-1)>>DYNAMAP_SHIFT);
    for (uintptr_t i=idx; i<=end; ++i) {
        uint32_t prot;
        do {
            prot=arm_lock_read_d(&memprot[i]);
        } while(arm_lock_write_d(&memprot[i], prot&~PROT_DYNAREC));
        if(prot&PROT_DYNAREC) {
            mprotect((void*)(i<<DYNAMAP_SHIFT), 1<<DYNAMAP_SHIFT, prot&~PROT_DYNAREC);
            cleanDBFromAddressRange((i<<DYNAMAP_SHIFT), 1<<DYNAMAP_SHIFT, 0);
        }
    }
}

#endif

void updateProtection(uintptr_t addr, uintptr_t size, uint32_t prot)
{
    const uintptr_t idx = (addr>>DYNAMAP_SHIFT);
    const uintptr_t end = ((addr+size-1)>>DYNAMAP_SHIFT);
    for (uintptr_t i=idx; i<=end; ++i) {
        #ifdef DYNAREC
        uint32_t dyn;
        do {
            dyn=arm_lock_read_d(&memprot[i])&PROT_DYNAREC;
        } while(arm_lock_write_d(&memprot[i], prot|dyn));
        if(dyn && (prot&PROT_WRITE))    // need to remove the write protection from this block
            mprotect((void*)(i<<DYNAMAP_SHIFT), 1<<DYNAMAP_SHIFT, prot&~PROT_WRITE);
        #else
        uint32_t dyn=(memprot[i]&PROT_DYNAREC);
        if(dyn && (prot&PROT_WRITE))    // need to remove the write protection from this block
            mprotect((void*)(i<<DYNAMAP_SHIFT), 1<<DYNAMAP_SHIFT, prot&~PROT_WRITE);
        memprot[i] = prot|dyn;
        #endif
    }
}

void setProtection(uintptr_t addr, uintptr_t size, uint32_t prot)
{
    const uintptr_t idx = (addr>>DYNAMAP_SHIFT);
    const uintptr_t end = ((addr+size-1)>>DYNAMAP_SHIFT);
    for (uintptr_t i=idx; i<=end; ++i) {
        memprot[i] = prot;
    }
}

uint32_t getProtection(uintptr_t addr)
{
    const uintptr_t idx = (addr>>DYNAMAP_SHIFT);
    return memprot[idx];
}

void init_custommem_helper(box86context_t* ctx)
{
    memprot = (uint32_t*)calloc(DYNAMAP_SIZE, sizeof(uint32_t));
#ifdef DYNAREC
    if(dynmap) // already initialized
        return;
    pthread_mutex_init(&mutex_mmap, NULL);
    dynmap = (dynablocklist_t**)calloc(DYNAMAP_SIZE, sizeof(dynablocklist_t*));
#ifdef ARM
    box86_jmptbl_default = (uintptr_t*)malloc((1<<DYNAMAP_SHIFT)*sizeof(uintptr_t));
    for(int i=0; i<(1<<DYNAMAP_SHIFT); ++i)
        box86_jmptbl_default[i] = (uintptr_t)arm_next;
    box86_jumptable = (uintptr_t**)malloc(DYNAMAP_SIZE*sizeof(uintptr_t*));
    for(int i=0; i<DYNAMAP_SIZE; ++i)
        box86_jumptable[i] = box86_jmptbl_default;
#else
#error Unsupported architecture!
#endif
#endif
}

void fini_custommem_helper(box86context_t *ctx)
{
#ifdef DYNAREC
    dynarec_log(LOG_DEBUG, "Free global Dynarecblocks\n");
    for (int i=0; i<mmapsize; ++i) {
        if(mmaplist[i].block)
            free(mmaplist[i].block);
        if(mmaplist[i].dblist) {
            kh_destroy(dynablocks, mmaplist[i].dblist);
            mmaplist[i].dblist = NULL;
        }
    }
    if(dblist_oversized) {
        kh_destroy(dynablocks, dblist_oversized);
        dblist_oversized = NULL;
    }
    mmapsize = 0;
    dynarec_log(LOG_DEBUG, "Free dynamic Dynarecblocks\n");
    uintptr_t idx = 0;
    uintptr_t end = ((0xFFFFFFFF)>>DYNAMAP_SHIFT);
    for (uintptr_t i=idx; i<=end; ++i) {
        dynablocklist_t* dblist = dynmap[i];
        if(dblist) {
            uintptr_t startdb = StartDynablockList(dblist);
            uintptr_t enddb = EndDynablockList(dblist);
            uintptr_t startaddr = 0;
            if(startaddr<startdb) startaddr = startdb;
            uintptr_t endaddr = 0xFFFFFFFF;
            if(endaddr>enddb) endaddr = enddb;
            FreeRangeDynablock(dblist, startaddr, endaddr-startaddr+1);
        }
    }
    for (uintptr_t i=idx; i<=end; ++i)
        if(dynmap[i])
            FreeDynablockList(&dynmap[i]);
    pthread_mutex_destroy(&mutex_mmap);
    free(mmaplist);
    free(dynmap);
    dynmap = NULL;
    for (int i=0; i<DYNAMAP_SIZE; ++i)
        if(box86_jumptable[i]!=box86_jmptbl_default)
            free(box86_jumptable[i]);
    free(box86_jumptable);
    free(box86_jmptbl_default);
    box86_jumptable = NULL;
    box86_jmptbl_default = NULL;
#endif
    free(memprot);
    memprot = NULL;
    for(int i=0; i<n_blocks; ++i)
        free(p_blocks[i].block);
    free(p_blocks);
    pthread_mutex_destroy(&mutex_blocks);
}
