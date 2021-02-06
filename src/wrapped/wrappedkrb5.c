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
#include "box86context.h"
#include "librarian.h"
#include "callback.h"

const char* krb5Name = "libkrb5.so.3";
#define LIBNAME krb5
static library_t *my_lib = NULL;

typedef int     (*iFppppppipp_t)      (void*, void*, void*, void*, void*, void*, int, void*, void*);

#define SUPER()                                     \
    GO(krb5_get_init_creds_password, iFppppppipp_t) \

typedef struct krb5_my_s {
    // functions
    #define GO(A, B)    B   A;
    SUPER()
    #undef GO
} krb5_my_t;

void* getKrb5My(library_t* lib)
{
    krb5_my_t* my = (krb5_my_t*)calloc(1, sizeof(krb5_my_t));
    #define GO(A, W) my->A = (W)dlsym(lib->priv.w.lib, #A);
    SUPER()
    #undef GO
    return my;
}
#undef SUPER

void freeKrb5My(void* lib)
{
    //krb5_my_t *my = (krb5_my_t *)lib;
}

#define SUPER() \
GO(0)   \
GO(1)   \
GO(2)   \
GO(3)   \
GO(4)

// krb5_prompter ...
#define GO(A)   \
static uintptr_t my_krb5_prompter_fct_##A = 0;                                      \
static int my_krb5_prompter_##A(void* a, void* b, void* c, void* d, int e, void* f) \
{                                                                                   \
    return RunFunction(my_context, my_krb5_prompter_fct_##A, 6, a, b, c, d, e, f);  \
}
SUPER()
#undef GO
static void* find_krb5_prompter_Fct(void* fct)
{
    if(!fct) return fct;
    if(GetNativeFnc((uintptr_t)fct))  return GetNativeFnc((uintptr_t)fct);
    #define GO(A) if(my_krb5_prompter_fct_##A == (uintptr_t)fct) return my_krb5_prompter_##A;
    SUPER()
    #undef GO
    #define GO(A) if(my_krb5_prompter_fct_##A == 0) {my_krb5_prompter_fct_##A = (uintptr_t)fct; return my_krb5_prompter_##A; }
    SUPER()
    #undef GO
    printf_log(LOG_NONE, "Warning, no more slot for libkrb5 krb5_prompter callback\n");
    return NULL;
}
#undef SUPER

EXPORT int my_krb5_get_init_creds_password(x86emu_t* emu, void* context, void* creds, void* client, void* password, void* f, void* data, int delta, void* service, void* options)
{
    krb5_my_t* my = (krb5_my_t*)my_lib->priv.w.p2;

    return my->krb5_get_init_creds_password(context, creds, client, password, find_krb5_prompter_Fct(f), data, delta, service, options);
}

#define CUSTOM_INIT \
    lib->priv.w.p2 = getKrb5My(lib);    \
    my_lib = lib;

#define CUSTOM_FINI \
    freeKrb5My(lib->priv.w.p2); \
    free(lib->priv.w.p2);       \
    my_lib = NULL;

#include "wrappedlib_init.h"

