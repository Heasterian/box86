#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <pthread.h>

#include "custommem.h"
#include "bridge.h"
#include "bridge_private.h"
#include "wrapper.h"
#include "khash.h"
#include "debug.h"
#include "x86emu.h"
#ifdef DYNAREC
#include "dynablock.h"
#include "box86context.h"
#endif

KHASH_MAP_INIT_INT(bridgemap, uintptr_t)

//onebrigde is 16 bytes
#define NBRICK  4096/16
typedef struct brick_s brick_t;
typedef struct brick_s {
    onebridge_t *b;
    int         sz;
    brick_t     *next;
} brick_t;

typedef struct bridge_s {
    brick_t         *head;
    brick_t         *last;      // to speed up
    pthread_mutex_t mutex;
    kh_bridgemap_t  *bridgemap;
} bridge_t;

brick_t* NewBrick()
{
    brick_t* ret = (brick_t*)calloc(1, sizeof(brick_t));
    posix_memalign((void**)&ret->b, box86_pagesize, NBRICK*sizeof(onebridge_t));
    return ret;
}

bridge_t *NewBridge()
{
    bridge_t *b = (bridge_t*)calloc(1, sizeof(bridge_t));
    b->head = NewBrick();
    b->last = b->head;
    pthread_mutex_init(&b->mutex, NULL);
    b->bridgemap = kh_init(bridgemap);

    return b;
}
void FreeBridge(bridge_t** bridge)
{
    brick_t *b = (*bridge)->head;
    while(b) {
        brick_t *n = b->next;
        #ifdef DYNAREC
        if(getProtection((uintptr_t)b->b)&PROT_DYNAREC)
            unprotectDB((uintptr_t)b->b, NBRICK*sizeof(onebridge_t));
        #endif
        free(b->b);
        free(b);
        b = n;
    }
    kh_destroy(bridgemap, (*bridge)->bridgemap);
    pthread_mutex_destroy(&(*bridge)->mutex);
    free(*bridge);
    *bridge = NULL;
}

uintptr_t AddBridge(bridge_t* bridge, wrapper_t w, void* fnc, int N)
{
    pthread_mutex_lock(&bridge->mutex);
    brick_t *b = bridge->last;
    if(b->sz == NBRICK) {
        b->next = NewBrick();
        b = b->next;
        bridge->last = b;
    }
    #ifdef DYNAREC
    int prot = 0;
    if(box86_dynarec) {
        prot=(getProtection((uintptr_t)b->b)&PROT_DYNAREC)?1:0;
        if(prot)
            unprotectDB((uintptr_t)b->b, NBRICK*sizeof(onebridge_t));
        addDBFromAddressRange((uintptr_t)&b->b[b->sz].CC, sizeof(onebridge_t));
    }
    #endif
    b->b[b->sz].CC = 0xCC;
    b->b[b->sz].S = 'S'; b->b[b->sz].C='C';
    b->b[b->sz].w = w;
    b->b[b->sz].f = (uintptr_t)fnc;
    b->b[b->sz].C3 = N?0xC2:0xC3;
    b->b[b->sz].N = N;
    #ifdef DYNAREC
    if(box86_dynarec && prot)
        protectDB((uintptr_t)b->b, NBRICK*sizeof(onebridge_t));
    #endif
    // add bridge to map, for fast recovery
    int ret;
    khint_t k = kh_put(bridgemap, bridge->bridgemap, (uintptr_t)fnc, &ret);
    kh_value(bridge->bridgemap, k) = (uintptr_t)&b->b[b->sz].CC;
    pthread_mutex_unlock(&bridge->mutex);

    return (uintptr_t)&b->b[b->sz++].CC;
}

uintptr_t CheckBridged(bridge_t* bridge, void* fnc)
{
    // check if function alread have a bridge (the function wrapper will not be tested)
    khint_t k = kh_get(bridgemap, bridge->bridgemap, (uint32_t)fnc);
    if(k==kh_end(bridge->bridgemap))
        return 0;
    return kh_value(bridge->bridgemap, k);
}

uintptr_t AddCheckBridge(bridge_t* bridge, wrapper_t w, void* fnc, int N)
{
    if(!fnc && w)
        return 0;
    uintptr_t ret = CheckBridged(bridge, fnc);
    if(!ret)
        ret = AddBridge(bridge, w, fnc, N);
    return ret;
}

uintptr_t AddAutomaticBridge(x86emu_t* emu, bridge_t* bridge, wrapper_t w, void* fnc, int N)
{
    if(!fnc)
        return 0;
    uintptr_t ret = CheckBridged(bridge, fnc);
    if(!ret)
        ret = AddBridge(bridge, w, fnc, N);
    if(!hasAlternate(fnc)) {
        printf_log(LOG_DEBUG, "Adding AutomaticBridge for %p to %p\n", fnc, (void*)ret);
        addAlternate(fnc, (void*)ret);
        #ifdef DYNAREC
        // now, check if dynablock at native address exist
        DBAlternateBlock(emu, (uintptr_t)fnc, ret);
        #endif
    }
    return ret;
}

void* GetNativeFnc(uintptr_t fnc)
{
    if(!fnc) return NULL;
    // check if function exist in some loaded lib
    Dl_info info;
    if(dladdr((void*)fnc, &info))
        return (void*)fnc;
    // check if it's an indirect jump
    #define PK(a)       *(uint8_t*)(fnc+a)
    #define PK32(a)     *(uint32_t*)(fnc+a)
    if(PK(0)==0xff && PK(1)==0x25) {  // absolute jump, maybe the GOT
        uintptr_t a1 = (PK32(2));   // need to add a check to see if the address is from the GOT !
        a1 = *(uintptr_t*)a1;
        if(a1 && a1>0x10000) {
            a1 = (uintptr_t)GetNativeFnc(a1);
            if(a1)
                return (void*)a1;
        }
    }
    #undef PK
    #undef PK32
    // check if bridge exist
    onebridge_t *b = (onebridge_t*)fnc;
    if(b->CC != 0xCC || b->S!='S' || b->C!='C' || (b->C3!=0xC3 && b->C3!=0xC2))
        return NULL;    // not a bridge?!
    return (void*)b->f;
}

void* GetNativeFncOrFnc(uintptr_t fnc)
{
    onebridge_t *b = (onebridge_t*)fnc;
    if(b->CC != 0xCC || b->S!='S' || b->C!='C' || (b->C3!=0xC3 && b->C3!=0xC2))
        return (void*)fnc;    // not a bridge?!
    return (void*)b->f;
}

// Alternate address handling
KHASH_MAP_INIT_INT(alternate, void*)
static kh_alternate_t *my_alternates = NULL;

int hasAlternate(void* addr) {
    if(!my_alternates)
        return 0;
    khint_t k = kh_get(alternate, my_alternates, (uintptr_t)addr);
    if(k==kh_end(my_alternates))
        return 0;
    return 1;
}

void* getAlternate(void* addr) {
    if(!my_alternates)
        return addr;
    khint_t k = kh_get(alternate, my_alternates, (uintptr_t)addr);
    if(k!=kh_end(my_alternates))
        return kh_value(my_alternates, k);
    return addr;
}
void addAlternate(void* addr, void* alt) {
    if(!my_alternates) {
        my_alternates = kh_init(alternate);
    }
    int ret;
    khint_t k = kh_put(alternate, my_alternates, (uintptr_t)addr, &ret);
    if(!ret)    // already there
        return;
    kh_value(my_alternates, k) = alt;
}

void cleanAlternate() {
    if(my_alternates) {
        kh_destroy(alternate, my_alternates);
        my_alternates = NULL;
    }
}
