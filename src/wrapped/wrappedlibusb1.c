#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define _GNU_SOURCE         /* See feature_test_macros(7) */
#include <dlfcn.h>

#include "wrappedlibs.h"

#include "debug.h"
#include "wrapper.h"
#include "bridge.h"
#include "librarian/library_private.h"
#include "x86emu.h"
#include "emu/x86emu_private.h"
#include "callback.h"
#include "librarian.h"
#include "box86context.h"
#include "emu/x86emu_private.h"
#include "myalign.h"

const char* libusb1Name = "libusb-1.0.so.0";
#define LIBNAME libusb1

typedef void*   (*pFi_t)            (int);
typedef int     (*iFp_t)            (void*);
typedef void    (*vFpu_t)           (void*, uint32_t);
typedef int     (*iFpiiiiippp_t)    (void*, int, int, int, int, int, void*, void*, void*);

static library_t* my_lib = NULL;

#define SUPER() \
    GO(libusb_hotplug_register_callback, iFpiiiiippp_t) \
    GO(libusb_alloc_transfer, pFi_t)                    \
    GO(libusb_submit_transfer, iFp_t)                   \
    GO(libusb_cancel_transfer, iFp_t)                   \

typedef struct libusb1_my_s {
    // functions
    #define GO(A, B)    B   A;
    SUPER()
    #undef GO
} libusb1_my_t;

void* getUsb1My(library_t* lib)
{
    libusb1_my_t* my = (libusb1_my_t*)calloc(1, sizeof(libusb1_my_t));
    #define GO(A, W) my->A = (W)dlsym(lib->priv.w.lib, #A);
    SUPER()
    #undef GO
    return my;
}
#undef SUPER

void freeUsb1My(void* lib)
{
//    libusb1_my_t *my = (libusb1_my_t *)lib;
}

#define SUPER() \
GO(0)   \
GO(1)   \
GO(2)   \
GO(3)   \
GO(4)   \
GO(5)   \
GO(6)   \
GO(7)   \
GO(8)   \
GO(9)   \


// hotplug
#define GO(A)   \
static uintptr_t my_hotplug_fct_##A = 0;                                                    \
static int my_hotplug_##A(void* ctx, void* device, int event, void* data)                   \
{                                                                                           \
    return (int)RunFunction(my_context, my_hotplug_fct_##A, 4, ctx, device, event, data);   \
}
SUPER()
#undef GO
static void* findhotplugFct(void* fct)
{
    if(!fct) return fct;
    if(GetNativeFnc((uintptr_t)fct))  return GetNativeFnc((uintptr_t)fct);
    #define GO(A) if(my_hotplug_fct_##A == (uintptr_t)fct) return my_hotplug_##A;
    SUPER()
    #undef GO
    #define GO(A) if(my_hotplug_fct_##A == 0) {my_hotplug_fct_##A = (uintptr_t)fct; return my_hotplug_##A; }
    SUPER()
    #undef GO
    printf_log(LOG_NONE, "Warning, no more slot for libusb-1.0 hotplug callback (%p)\n", fct);
    return NULL;
}
// transfert
#define GO(A)   \
static uintptr_t my_transfert_fct_##A = 0;                      \
static void my_transfert_##A(void* ctx)                         \
{                                                               \
    RunFunction(my_context, my_transfert_fct_##A, 1, ctx);      \
}
SUPER()
#undef GO
static void* findtransfertFct(void* fct)
{
    if(!fct) return fct;
    #define GO(A) if(my_transfert_##A == fct) return my_transfert_##A;
    SUPER()
    #undef GO
    if(GetNativeFnc((uintptr_t)fct))  return GetNativeFnc((uintptr_t)fct);
    #define GO(A) if(my_transfert_fct_##A == (uintptr_t)fct) return my_transfert_##A;
    SUPER()
    #undef GO
    #define GO(A) if(my_transfert_fct_##A == 0) {my_transfert_fct_##A = (uintptr_t)fct; return my_transfert_##A; }
    SUPER()
    #undef GO
    printf_log(LOG_NONE, "Warning, no more slot for libusb-1.0 transfert callback (%p)\n", fct);
    return NULL;
}
static void* reverse_transfert_Fct(void* fct)
{
    if(!fct) return fct;
    if(CheckBridged(my_lib->priv.w.bridge, fct))
        return (void*)CheckBridged(my_lib->priv.w.bridge, fct);
    #define GO(A) if(my_transfert_##A == fct) return (void*)my_transfert_fct_##A;
    SUPER()
    #undef GO
    return (void*)AddBridge(my_lib->priv.w.bridge, vFp, fct, 0);
}

#undef SUPER

EXPORT int my_libusb_hotplug_register_callback(x86emu_t* emu, void* ctx, int event, int flags, int vendor, int product, int dev_class, void* f, void* data, void* handle)
{
    libusb1_my_t *my = (libusb1_my_t*)my_lib->priv.w.p2;

    return my->libusb_hotplug_register_callback(ctx, event, flags, vendor, product, dev_class, findhotplugFct(f), data, handle);
}

struct my_libusb_iso_packet_descriptor {
    unsigned int length;
    unsigned int actual_length;
    int status;
};

typedef struct my_libusb_transfer_s {
    void* dev_handle;
    uint8_t flags;
    unsigned char endpoint;
    unsigned char type;
    unsigned int timeout;
    int status;
    int length;
    int actual_length;
    void* callback;
    void* user_data;
    unsigned char *buffer;
    int num_iso_packets;
    struct my_libusb_iso_packet_descriptor iso_packet_desc[0];
} my_libusb_transfer_t;

EXPORT void* my_libusb_alloc_transfer(x86emu_t* emu, int num)
{
    libusb1_my_t *my = (libusb1_my_t*)my_lib->priv.w.p2;

    my_libusb_transfer_t* ret = (my_libusb_transfer_t*)my->libusb_alloc_transfer(num);
    if(ret)
        ret->callback = reverse_transfert_Fct(ret->callback);
    return ret;
}

EXPORT int my_libusb_submit_transfer(x86emu_t* emu, my_libusb_transfer_t* t)
{
    libusb1_my_t *my = (libusb1_my_t*)my_lib->priv.w.p2;
    
    t->callback = findtransfertFct(t->callback);
    return my->libusb_submit_transfer(t); // don't put back callback, it's unknown if it's safe
} 

EXPORT int my_libusb_cancel_transfer(x86emu_t* emu, my_libusb_transfer_t* t)
{
    libusb1_my_t *my = (libusb1_my_t*)my_lib->priv.w.p2;
    
    t->callback = findtransfertFct(t->callback);
    return my->libusb_cancel_transfer(t);
}

#define CUSTOM_INIT \
    my_lib = lib;   \
    lib->priv.w.p2 = getUsb1My(lib);

#define CUSTOM_FINI \
    freeUsb1My(lib->priv.w.p2); \
    free(lib->priv.w.p2);

#include "wrappedlib_init.h"

