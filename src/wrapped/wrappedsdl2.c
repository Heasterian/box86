#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define _GNU_SOURCE         /* See feature_test_macros(7) */
#include <dlfcn.h>
#include <stdarg.h>

#include "wrappedlibs.h"

#include "debug.h"
#include "wrapper.h"
#include "bridge.h"
#include "callback.h"
#include "librarian.h"
#include "librarian/library_private.h"
#include "emu/x86emu_private.h"
#include "box86context.h"
#include "sdl2rwops.h"
#include "myalign.h"
#include "threads.h"

static int sdl_Yes() { return 1;}
static int sdl_No() { return 0;}
int EXPORT my2_SDL_Has3DNow() __attribute__((alias("sdl_No")));
int EXPORT my2_SDL_Has3DNowExt() __attribute__((alias("sdl_No")));
int EXPORT my2_SDL_HasAltiVec() __attribute__((alias("sdl_No")));
int EXPORT my2_SDL_HasMMX() __attribute__((alias("sdl_Yes")));
int EXPORT my2_SDL_HasMMXExt() __attribute__((alias("sdl_Yes")));
int EXPORT my2_SDL_HasNEON() __attribute__((alias("sdl_No")));   // No neon in x86 ;)
int EXPORT my2_SDL_HasRDTSC() __attribute__((alias("sdl_Yes")));
int EXPORT my2_SDL_HasSSE() __attribute__((alias("sdl_Yes")));
int EXPORT my2_SDL_HasSSE2() __attribute__((alias("sdl_Yes")));
int EXPORT my2_SDL_HasSSE3() __attribute__((alias("sdl_Yes")));
int EXPORT my2_SDL_HasSSE41() __attribute__((alias("sdl_No")));
int EXPORT my2_SDL_HasSSE42() __attribute__((alias("sdl_No")));

typedef struct {
  int32_t freq;
  uint16_t format;
  uint8_t channels;
  uint8_t silence;
  uint16_t samples;
  uint32_t size;
  void (*callback)(void *userdata, uint8_t *stream, int32_t len);
  void *userdata;
} SDL2_AudioSpec;

KHASH_MAP_INIT_INT(timercb, x86emu_t*)

typedef struct {
    uint8_t data[16];
} SDL_JoystickGUID;

typedef union {
    SDL_JoystickGUID guid;
    uint32_t         u[4];
} SDL_JoystickGUID_Helper;

typedef struct
{
    int32_t bindType;   // enum
    union
    {
        int button;
        int axis;
        struct {
            int hat;
            int hat_mask;
        } hat;
    } value;
} SDL_GameControllerButtonBind;


// TODO: put the wrapper type in a dedicate include
typedef void  (*vFv_t)();
typedef void* (*pFv_t)();
typedef int32_t (*iFp_t)(void*);
typedef int32_t (*iFip_t)(int32_t, void*);
typedef int32_t (*iFWW_t)(uint16_t, uint16_t);
typedef int32_t (*iFS_t)(SDL_JoystickGUID);
typedef void* (*pFpi_t)(void*, int32_t);
typedef void* (*pFp_t)(void*);
typedef void* (*pFS_t)(SDL_JoystickGUID);
typedef void* (*pFpp_t)(void*, void*);
typedef int32_t (*iFppi_t)(void*, void*, int32_t);
typedef int32_t (*iFpippi_t)(void*, int32_t, void*, void*, int32_t);
typedef int32_t (*iFppp_t)(void*, void*, void*);
typedef void* (*pFpippp_t)(void*, int32_t, void*, void*, void*);
typedef void*  (*pFpp_t)(void*, void*);
typedef void*  (*pFppp_t)(void*, void*, void*);
typedef void  (*vFp_t)(void*);
typedef void  (*vFpp_t)(void*, void*);
typedef void  (*vFSppp_t)(SDL_JoystickGUID, void*, void*, void*);
typedef void  (*vFiupp_t)(int32_t, uint32_t, void*, void*);
typedef int32_t (*iFpupp_t)(void*, uint32_t, void*, void*);
typedef uint32_t (*uFu_t)(uint32_t);
typedef uint32_t (*uFp_t)(void*);
typedef uint32_t (*uFupp_t)(uint32_t, void*, void*);
typedef int64_t (*IFp_t)(void*);
typedef uint64_t (*UFp_t)(void*);
typedef int32_t (*iFpi_t)(void*, int32_t);
typedef int32_t (*iFpp_t)(void*, void*);
typedef int32_t (*iFupp_t)(uint32_t, void*, void*);
typedef uint32_t (*uFpC_t)(void*, uint8_t);
typedef uint32_t (*uFpW_t)(void*, uint16_t);
typedef uint32_t (*uFpu_t)(void*, uint32_t);
typedef uint32_t (*uFpU_t)(void*, uint64_t);
typedef SDL_JoystickGUID (*SFi_t)(int32_t);
typedef SDL_JoystickGUID (*SFp_t)(void*);
typedef SDL_GameControllerButtonBind (*SFpi_t)(void*, int32_t);

#define SUPER() \
    GO(SDL_Quit, vFv_t)                             \
    GO(SDL_OpenAudio, iFpp_t)                       \
    GO(SDL_OpenAudioDevice, iFpippi_t)              \
    GO(SDL_LoadFile_RW, pFpi_t)                     \
    GO(SDL_LoadBMP_RW, pFpi_t)                      \
    GO(SDL_RWFromConstMem, pFpi_t)                  \
    GO(SDL_RWFromFP, pFpi_t)                        \
    GO(SDL_RWFromFile, pFpp_t)                      \
    GO(SDL_RWFromMem, pFpi_t)                       \
    GO(SDL_SaveBMP_RW, iFppi_t)                     \
    GO(SDL_LoadWAV_RW, pFpippp_t)                   \
    GO(SDL_GameControllerAddMappingsFromRW, iFpi_t) \
    GO(SDL_AllocRW, sdl2_allocrw)                   \
    GO(SDL_FreeRW, sdl2_freerw)                     \
    GO(SDL_ReadU8, uFp_t)                           \
    GO(SDL_ReadBE16, uFp_t)                         \
    GO(SDL_ReadBE32, uFp_t)                         \
    GO(SDL_ReadBE64, UFp_t)                         \
    GO(SDL_ReadLE16, uFp_t)                         \
    GO(SDL_ReadLE32, uFp_t)                         \
    GO(SDL_ReadLE64, UFp_t)                         \
    GO(SDL_WriteU8, uFpC_t)                         \
    GO(SDL_WriteBE16, uFpW_t)                       \
    GO(SDL_WriteBE32, uFpu_t)                       \
    GO(SDL_WriteBE64, uFpU_t)                       \
    GO(SDL_WriteLE16, uFpW_t)                       \
    GO(SDL_WriteLE32, uFpu_t)                       \
    GO(SDL_WriteLE64, uFpU_t)                       \
    GO(SDL_AddTimer, uFupp_t)                       \
    GO(SDL_RemoveTimer, uFu_t)                      \
    GO(SDL_CreateThread, pFppp_t)                   \
    GO(SDL_KillThread, vFp_t)                       \
    GO(SDL_GetEventFilter, iFpp_t)                  \
    GO(SDL_SetEventFilter, vFpp_t)                  \
    GO(SDL_LogGetOutputFunction, vFpp_t)            \
    GO(SDL_LogSetOutputFunction, vFpp_t)            \
    GO(SDL_LogMessageV, vFiupp_t)                   \
    GO(SDL_GL_GetProcAddress, pFp_t)                \
    GO(SDL_TLSSet, iFupp_t)                         \
    GO(SDL_JoystickGetDeviceGUID, SFi_t)            \
    GO(SDL_JoystickGetGUID, SFp_t)                  \
    GO(SDL_JoystickGetGUIDFromString, SFp_t)        \
    GO(SDL_GameControllerGetBindForAxis, SFpi_t)    \
    GO(SDL_GameControllerGetBindForButton, SFpi_t)  \
    GO(SDL_AddEventWatch, vFpp_t)                   \
    GO(SDL_DelEventWatch, vFpp_t)                   \
    GO(SDL_GameControllerMappingForGUID, pFS_t)     \
    GO(SDL_SaveAllDollarTemplates, iFp_t)           \
    GO(SDL_SaveDollarTemplate, iFip_t)              \
    GO(SDL_GetJoystickGUIDInfo, vFSppp_t)           \
    GO(SDL_IsJoystickPS4, iFWW_t)                   \
    GO(SDL_IsJoystickNintendoSwitchPro, iFWW_t)     \
    GO(SDL_IsJoystickSteamController, iFWW_t)       \
    GO(SDL_IsJoystickXbox360, iFWW_t)               \
    GO(SDL_IsJoystickXboxOne, iFWW_t)               \
    GO(SDL_IsJoystickXInput, iFS_t)                 \
    GO(SDL_IsJoystickHIDAPI, iFS_t)                 \
    GO(SDL_Vulkan_GetVkGetInstanceProcAddr, pFv_t)  \

typedef struct sdl2_my_s {
    #define GO(A, B)    B   A;
    SUPER()
    #undef GO
} sdl2_my_t;

void* getSDL2My(library_t* lib)
{
    sdl2_my_t* my = (sdl2_my_t*)calloc(1, sizeof(sdl2_my_t));
    #define GO(A, W) my->A = (W)dlsym(lib->priv.w.lib, #A);
    SUPER()
    #undef GO
    return my;
}

void freeSDL2My(void* lib)
{
    /*sdl2_my_t *my = (sdl2_my_t *)lib;*/
}
#undef SUPER

#define SUPER() \
GO(0)   \
GO(1)   \
GO(2)   \
GO(3)   \
GO(4)

// Timer
#define GO(A)   \
static uintptr_t my_Timer_fct_##A = 0;                                      \
static uint32_t my_Timer_##A(uint32_t a, void* b)                           \
{                                                                           \
    return (uint32_t)RunFunction(my_context, my_Timer_fct_##A, 2, a, b);    \
}
SUPER()
#undef GO
static void* find_Timer_Fct(void* fct)
{
    if(!fct) return NULL;
    void* p;
    if((p = GetNativeFnc((uintptr_t)fct))) return p;
    #define GO(A) if(my_Timer_fct_##A == (uintptr_t)fct) return my_Timer_##A;
    SUPER()
    #undef GO
    #define GO(A) if(my_Timer_fct_##A == 0) {my_Timer_fct_##A = (uintptr_t)fct; return my_Timer_##A; }
    SUPER()
    #undef GO
    printf_log(LOG_NONE, "Warning, no more slot for SDL2 Timer callback\n");
    return NULL;
    
}
// AudioCallback
#define GO(A)   \
static uintptr_t my_AudioCallback_fct_##A = 0;                      \
static void my_AudioCallback_##A(void* a, void* b, int c)           \
{                                                                   \
    RunFunction(my_context, my_AudioCallback_fct_##A, 3, a, b, c);  \
}
SUPER()
#undef GO
static void* find_AudioCallback_Fct(void* fct)
{
    if(!fct) return NULL;
    void* p;
    if((p = GetNativeFnc((uintptr_t)fct))) return p;
    #define GO(A) if(my_AudioCallback_fct_##A == (uintptr_t)fct) return my_AudioCallback_##A;
    SUPER()
    #undef GO
    #define GO(A) if(my_AudioCallback_fct_##A == 0) {my_AudioCallback_fct_##A = (uintptr_t)fct; return my_AudioCallback_##A; }
    SUPER()
    #undef GO
    printf_log(LOG_NONE, "Warning, no more slot for SDL2 AudioCallback callback\n");
    return NULL;
    
}
// eventfilter
#define GO(A)   \
static uintptr_t my_eventfilter_fct_##A = 0;                                \
static int my_eventfilter_##A(void* userdata, void* event)                  \
{                                                                           \
    return (int)RunFunction(my_context, my_eventfilter_fct_##A, 2, userdata, event);    \
}
SUPER()
#undef GO
static void* find_eventfilter_Fct(void* fct)
{
    if(!fct) return NULL;
    void* p;
    if((p = GetNativeFnc((uintptr_t)fct))) return p;
    #define GO(A) if(my_eventfilter_fct_##A == (uintptr_t)fct) return my_eventfilter_##A;
    SUPER()
    #undef GO
    #define GO(A) if(my_eventfilter_fct_##A == 0) {my_eventfilter_fct_##A = (uintptr_t)fct; return my_eventfilter_##A; }
    SUPER()
    #undef GO
    printf_log(LOG_NONE, "Warning, no more slot for SDL2 eventfilter callback\n");
    return NULL;
    
}
static void* reverse_eventfilter_Fct(void* fct)
{
    if(!fct) return fct;
    if(CheckBridged(my_context->sdl2lib->priv.w.bridge, fct))
        return (void*)CheckBridged(my_context->sdl2lib->priv.w.bridge, fct);
    #define GO(A) if(my_eventfilter_##A == fct) return (void*)my_eventfilter_fct_##A;
    SUPER()
    #undef GO
    return (void*)AddBridge(my_context->sdl2lib->priv.w.bridge, iFpp, fct, 0);
}

// LogOutput
#define GO(A)   \
static uintptr_t my_LogOutput_fct_##A = 0;                                  \
static void my_LogOutput_##A(void* a, int b, int c, void* d)                \
{                                                                           \
    RunFunction(my_context, my_LogOutput_fct_##A, 4, a, b, c, d);  \
}
SUPER()
#undef GO
static void* find_LogOutput_Fct(void* fct)
{
    if(!fct) return fct;
    if(GetNativeFnc((uintptr_t)fct))  return GetNativeFnc((uintptr_t)fct);
    #define GO(A) if(my_LogOutput_fct_##A == (uintptr_t)fct) return my_LogOutput_##A;
    SUPER()
    #undef GO
    #define GO(A) if(my_LogOutput_fct_##A == 0) {my_LogOutput_fct_##A = (uintptr_t)fct; return my_LogOutput_##A; }
    SUPER()
    #undef GO
    printf_log(LOG_NONE, "Warning, no more slot for SDL2 LogOutput callback\n");
    return NULL;
}
static void* reverse_LogOutput_Fct(void* fct)
{
    if(!fct) return fct;
    if(CheckBridged(my_context->sdl2lib->priv.w.bridge, fct))
        return (void*)CheckBridged(my_context->sdl2lib->priv.w.bridge, fct);
    #define GO(A) if(my_LogOutput_##A == fct) return (void*)my_LogOutput_fct_##A;
    SUPER()
    #undef GO
    return (void*)AddBridge(my_context->sdl2lib->priv.w.bridge, vFpiip, fct, 0);
}

#undef SUPER

// TODO: track the memory for those callback
EXPORT int32_t my2_SDL_OpenAudio(x86emu_t* emu, void* d, void* o)
{
    SDL2_AudioSpec *desired = (SDL2_AudioSpec*)d;

    sdl2_my_t *my = (sdl2_my_t *)emu->context->sdl2lib->priv.w.p2;
    // create a callback
    void *fnc = (void*)desired->callback;
    desired->callback = find_AudioCallback_Fct(fnc);
    int ret = my->SDL_OpenAudio(desired, (SDL2_AudioSpec*)o);
    if (ret!=0) {
        // error, clean the callback...
        desired->callback = fnc;
        return ret;
    }
    // put back stuff in place?
    desired->callback = fnc;

    return ret;
}

EXPORT int32_t my2_SDL_OpenAudioDevice(x86emu_t* emu, void* device, int32_t iscapture, void* d, void* o, int32_t allowed)
{
    SDL2_AudioSpec *desired = (SDL2_AudioSpec*)d;

    sdl2_my_t *my = (sdl2_my_t *)emu->context->sdl2lib->priv.w.p2;
    // create a callback
    void *fnc = (void*)desired->callback;
    desired->callback = find_AudioCallback_Fct(fnc);
    int ret = my->SDL_OpenAudioDevice(device, iscapture, desired, (SDL2_AudioSpec*)o, allowed);
    if (ret<=0) {
        // error, clean the callback...
        desired->callback = fnc;
        return ret;
    }
    // put back stuff in place?
    desired->callback = fnc;

    return ret;
}

EXPORT void *my2_SDL_LoadFile_RW(x86emu_t* emu, void* a, int b)
{
    sdl2_my_t *my = (sdl2_my_t *)emu->context->sdl2lib->priv.w.p2;
    SDL2_RWops_t *rw = RWNativeStart2(emu, (SDL2_RWops_t*)a);
    void* r = my->SDL_LoadFile_RW(rw, b);
    if(b==0)
        RWNativeEnd2(rw);
    return r;
}
EXPORT void *my2_SDL_LoadBMP_RW(x86emu_t* emu, void* a, int b)
{
    sdl2_my_t *my = (sdl2_my_t *)emu->context->sdl2lib->priv.w.p2;
    SDL2_RWops_t *rw = RWNativeStart2(emu, (SDL2_RWops_t*)a);
    void* r = my->SDL_LoadBMP_RW(rw, b);
    if(b==0)
        RWNativeEnd2(rw);
    return r;
}
EXPORT int32_t my2_SDL_SaveBMP_RW(x86emu_t* emu, void* a, void* b, int c)
{
    sdl2_my_t *my = (sdl2_my_t *)emu->context->sdl2lib->priv.w.p2;
    SDL2_RWops_t *rw = RWNativeStart2(emu, (SDL2_RWops_t*)a);
    int32_t r = my->SDL_SaveBMP_RW(rw, b, c);
    if(c==0)
        RWNativeEnd2(rw);
    return r;
}
EXPORT void *my2_SDL_LoadWAV_RW(x86emu_t* emu, void* a, int b, void* c, void* d, void* e)
{
    sdl2_my_t *my = (sdl2_my_t *)emu->context->sdl2lib->priv.w.p2;
    SDL2_RWops_t *rw = RWNativeStart2(emu, (SDL2_RWops_t*)a);
    void* r = my->SDL_LoadWAV_RW(rw, b, c, d, e);
    if(b==0)
        RWNativeEnd2(rw);
    return r;
}
EXPORT int32_t my2_SDL_GameControllerAddMappingsFromRW(x86emu_t* emu, void* a, int b)
{
    sdl2_my_t *my = (sdl2_my_t *)emu->context->sdl2lib->priv.w.p2;
    SDL2_RWops_t *rw = RWNativeStart2(emu, (SDL2_RWops_t*)a);
    int32_t r = my->SDL_GameControllerAddMappingsFromRW(rw, b);
    if(b==0)
        RWNativeEnd2(rw);
    return r;
}
EXPORT uint32_t my2_SDL_ReadU8(x86emu_t* emu, void* a)
{
    sdl2_my_t *my = (sdl2_my_t *)emu->context->sdl2lib->priv.w.p2;
    SDL2_RWops_t *rw = RWNativeStart2(emu, (SDL2_RWops_t*)a);
    uint32_t r = my->SDL_ReadU8(rw);
    RWNativeEnd2(rw);
    return r;
}
EXPORT uint32_t my2_SDL_ReadBE16(x86emu_t* emu, void* a)
{
    sdl2_my_t *my = (sdl2_my_t *)emu->context->sdl2lib->priv.w.p2;
    SDL2_RWops_t *rw = RWNativeStart2(emu, (SDL2_RWops_t*)a);
    uint32_t r = my->SDL_ReadBE16(rw);
    RWNativeEnd2(rw);
    return r;
}
EXPORT uint32_t my2_SDL_ReadBE32(x86emu_t* emu, void* a)
{
    sdl2_my_t *my = (sdl2_my_t *)emu->context->sdl2lib->priv.w.p2;
    SDL2_RWops_t *rw = RWNativeStart2(emu, (SDL2_RWops_t*)a);
    uint32_t r = my->SDL_ReadBE32(rw);
    RWNativeEnd2(rw);
    return r;
}
EXPORT uint64_t my2_SDL_ReadBE64(x86emu_t* emu, void* a)
{
    sdl2_my_t *my = (sdl2_my_t *)emu->context->sdl2lib->priv.w.p2;
    SDL2_RWops_t *rw = RWNativeStart2(emu, (SDL2_RWops_t*)a);
    uint64_t r = my->SDL_ReadBE64(rw);
    RWNativeEnd2(rw);
    return r;
}
EXPORT uint32_t my2_SDL_ReadLE16(x86emu_t* emu, void* a)
{
    sdl2_my_t *my = (sdl2_my_t *)emu->context->sdl2lib->priv.w.p2;
    SDL2_RWops_t *rw = RWNativeStart2(emu, (SDL2_RWops_t*)a);
    uint32_t r = my->SDL_ReadLE16(rw);
    RWNativeEnd2(rw);
    return r;
}
EXPORT uint32_t my2_SDL_ReadLE32(x86emu_t* emu, void* a)
{
    sdl2_my_t *my = (sdl2_my_t *)emu->context->sdl2lib->priv.w.p2;
    SDL2_RWops_t *rw = RWNativeStart2(emu, (SDL2_RWops_t*)a);
    uint32_t r = my->SDL_ReadLE32(rw);
    RWNativeEnd2(rw);
    return r;
}
EXPORT uint64_t my2_SDL_ReadLE64(x86emu_t* emu, void* a)
{
    sdl2_my_t *my = (sdl2_my_t *)emu->context->sdl2lib->priv.w.p2;
    SDL2_RWops_t *rw = RWNativeStart2(emu, (SDL2_RWops_t*)a);
    uint64_t r = my->SDL_ReadLE64(rw);
    RWNativeEnd2(rw);
    return r;
}
EXPORT uint32_t my2_SDL_WriteU8(x86emu_t* emu, void* a, uint8_t v)
{
    sdl2_my_t *my = (sdl2_my_t *)emu->context->sdl2lib->priv.w.p2;
    SDL2_RWops_t *rw = RWNativeStart2(emu, (SDL2_RWops_t*)a);
    uint32_t r = my->SDL_WriteU8(rw, v);
    RWNativeEnd2(rw);
    return r;
}
EXPORT uint32_t my2_SDL_WriteBE16(x86emu_t* emu, void* a, uint16_t v)
{
    sdl2_my_t *my = (sdl2_my_t *)emu->context->sdl2lib->priv.w.p2;
    SDL2_RWops_t *rw = RWNativeStart2(emu, (SDL2_RWops_t*)a);
    uint32_t r = my->SDL_WriteBE16(rw, v);
    RWNativeEnd2(rw);
    return r;
}
EXPORT uint32_t my2_SDL_WriteBE32(x86emu_t* emu, void* a, uint32_t v)
{
    sdl2_my_t *my = (sdl2_my_t *)emu->context->sdl2lib->priv.w.p2;
    SDL2_RWops_t *rw = RWNativeStart2(emu, (SDL2_RWops_t*)a);
    uint32_t r = my->SDL_WriteBE32(rw, v);
    RWNativeEnd2(rw);
    return r;
}
EXPORT uint32_t my2_SDL_WriteBE64(x86emu_t* emu, void* a, uint64_t v)
{
    sdl2_my_t *my = (sdl2_my_t *)emu->context->sdl2lib->priv.w.p2;
    SDL2_RWops_t *rw = RWNativeStart2(emu, (SDL2_RWops_t*)a);
    uint32_t r = my->SDL_WriteBE64(rw, v);
    RWNativeEnd2(rw);
    return r;
}
EXPORT uint32_t my2_SDL_WriteLE16(x86emu_t* emu, void* a, uint16_t v)
{
    sdl2_my_t *my = (sdl2_my_t *)emu->context->sdl2lib->priv.w.p2;
    SDL2_RWops_t *rw = RWNativeStart2(emu, (SDL2_RWops_t*)a);
    uint32_t r = my->SDL_WriteLE16(rw, v);
    RWNativeEnd2(rw);
    return r;
}
EXPORT uint32_t my2_SDL_WriteLE32(x86emu_t* emu, void* a, uint32_t v)
{
    sdl2_my_t *my = (sdl2_my_t *)emu->context->sdl2lib->priv.w.p2;
    SDL2_RWops_t *rw = RWNativeStart2(emu, (SDL2_RWops_t*)a);
    uint32_t r = my->SDL_WriteLE32(rw, v);
    RWNativeEnd2(rw);
    return r;
}
EXPORT uint32_t my2_SDL_WriteLE64(x86emu_t* emu, void* a, uint64_t v)
{
    sdl2_my_t *my = (sdl2_my_t *)emu->context->sdl2lib->priv.w.p2;
    SDL2_RWops_t *rw = RWNativeStart2(emu, (SDL2_RWops_t*)a);
    uint32_t r = my->SDL_WriteLE64(rw, v);
    RWNativeEnd2(rw);
    return r;
}

EXPORT void *my2_SDL_RWFromConstMem(x86emu_t* emu, void* a, int b)
{
    sdl2_my_t *my = (sdl2_my_t *)emu->context->sdl2lib->priv.w.p2;
    void* r = my->SDL_RWFromConstMem(a, b);
    return AddNativeRW2(emu, (SDL2_RWops_t*)r);
}
EXPORT void *my2_SDL_RWFromFP(x86emu_t* emu, void* a, int b)
{
    sdl2_my_t *my = (sdl2_my_t *)emu->context->sdl2lib->priv.w.p2;
    void* r = my->SDL_RWFromFP(a, b);
    return AddNativeRW2(emu, (SDL2_RWops_t*)r);
}
EXPORT void *my2_SDL_RWFromFile(x86emu_t* emu, void* a, void* b)
{
    sdl2_my_t *my = (sdl2_my_t *)emu->context->sdl2lib->priv.w.p2;
    void* r = my->SDL_RWFromFile(a, b);
    return AddNativeRW2(emu, (SDL2_RWops_t*)r);
}
EXPORT void *my2_SDL_RWFromMem(x86emu_t* emu, void* a, int b)
{
    sdl2_my_t *my = (sdl2_my_t *)emu->context->sdl2lib->priv.w.p2;
    void* r = my->SDL_RWFromMem(a, b);
    return AddNativeRW2(emu, (SDL2_RWops_t*)r);
}

EXPORT int64_t my2_SDL_RWseek(x86emu_t* emu, void* a, int64_t offset, int32_t whence)
{
    //sdl2_my_t *my = (sdl2_my_t *)emu->context->sdl2lib->priv.w.p2;
    SDL2_RWops_t *rw = RWNativeStart2(emu, (SDL2_RWops_t*)a);
    int64_t ret = RWNativeSeek2(rw, offset, whence);
    RWNativeEnd2(rw);
    return ret;
}
EXPORT int64_t my2_SDL_RWtell(x86emu_t* emu, void* a)
{
    //sdl2_my_t *my = (sdl2_my_t *)emu->context->sdl2lib->priv.w.p2;
    SDL2_RWops_t *rw = RWNativeStart2(emu, (SDL2_RWops_t*)a);
    int64_t ret = RWNativeSeek2(rw, 0, 1);  //1 == RW_SEEK_CUR
    RWNativeEnd2(rw);
    return ret;
}
EXPORT uint32_t my2_SDL_RWread(x86emu_t* emu, void* a, void* ptr, uint32_t size, uint32_t maxnum)
{
    //sdl2_my_t *my = (sdl2_my_t *)emu->context->sdl2lib->priv.w.p2;
    SDL2_RWops_t *rw = RWNativeStart2(emu, (SDL2_RWops_t*)a);
    uint32_t ret = RWNativeRead2(rw, ptr, size, maxnum);
    RWNativeEnd2(rw);
    return ret;
}
EXPORT uint32_t my2_SDL_RWwrite(x86emu_t* emu, void* a, const void* ptr, uint32_t size, uint32_t maxnum)
{
    //sdl2_my_t *my = (sdl2_my_t *)emu->context->sdl2lib->priv.w.p2;
    SDL2_RWops_t *rw = RWNativeStart2(emu, (SDL2_RWops_t*)a);
    uint32_t ret = RWNativeWrite2(rw, ptr, size, maxnum);
    RWNativeEnd2(rw);
    return ret;
}
EXPORT int my2_SDL_RWclose(x86emu_t* emu, void* a)
{
    //sdl2_my_t *my = (sdl2_my_t *)emu->context->sdl2lib->priv.w.p2;
    SDL2_RWops_t *rw = RWNativeStart2(emu, (SDL2_RWops_t*)a);
    return RWNativeClose2(rw);
}

EXPORT int my2_SDL_SaveAllDollarTemplates(x86emu_t* emu, void* a)
{
    sdl2_my_t *my = (sdl2_my_t *)emu->context->sdl2lib->priv.w.p2;
    SDL2_RWops_t *rw = RWNativeStart2(emu, (SDL2_RWops_t*)a);
    uint32_t ret = my->SDL_SaveAllDollarTemplates(rw);
    RWNativeEnd2(rw);
    return ret;
}

EXPORT int my2_SDL_SaveDollarTemplate(x86emu_t* emu, int gesture, void* a)
{
    sdl2_my_t *my = (sdl2_my_t *)emu->context->sdl2lib->priv.w.p2;
    SDL2_RWops_t *rw = RWNativeStart2(emu, (SDL2_RWops_t*)a);
    uint32_t ret = my->SDL_SaveDollarTemplate(gesture, rw);
    RWNativeEnd2(rw);
    return ret;
}

EXPORT uint32_t my2_SDL_AddTimer(x86emu_t* emu, uint32_t a, void* f, void* p)
{
    sdl2_my_t *my = (sdl2_my_t *)emu->context->sdl2lib->priv.w.p2;
    return my->SDL_AddTimer(a, find_Timer_Fct(f), p);
}

EXPORT void my2_SDL_RemoveTimer(x86emu_t* emu, uint32_t t)
{
    sdl2_my_t *my = (sdl2_my_t *)emu->context->sdl2lib->priv.w.p2;
    my->SDL_RemoveTimer(t);
}

EXPORT void my2_SDL_SetEventFilter(x86emu_t* emu, void* p, void* userdata)
{
    sdl2_my_t *my = (sdl2_my_t *)emu->context->sdl2lib->priv.w.p2;
    my->SDL_SetEventFilter(find_eventfilter_Fct(p), userdata);
}
EXPORT int my2_SDL_GetEventFilter(x86emu_t* emu, void** f, void* userdata)
{
    sdl2_my_t *my = (sdl2_my_t *)emu->context->sdl2lib->priv.w.p2;
    int ret = my->SDL_GetEventFilter(f, userdata);
    if(*f) reverse_eventfilter_Fct(*f);
    return ret;
}

EXPORT void my2_SDL_LogGetOutputFunction(x86emu_t* emu, void** f, void* arg)
{
    sdl2_my_t *my = (sdl2_my_t *)emu->context->sdl2lib->priv.w.p2;

    my->SDL_LogGetOutputFunction(f, arg);
    if(*f) *f = reverse_LogOutput_Fct(*f);
}
EXPORT void my2_SDL_LogSetOutputFunction(x86emu_t* emu, void* f, void* arg)
{
    sdl2_my_t *my = (sdl2_my_t *)emu->context->sdl2lib->priv.w.p2;

    my->SDL_LogSetOutputFunction(find_LogOutput_Fct(f), arg);
}

EXPORT int my2_SDL_vsnprintf(x86emu_t* emu, void* buff, uint32_t s, void * fmt, void * b, va_list V)
{
    #ifndef NOALIGN
    // need to align on arm
    myStackAlign((const char*)fmt, *(uint32_t**)b, emu->scratch);
    PREPARE_VALIST;
    void* f = vsnprintf;
    int r = ((iFpupp_t)f)(buff, s, fmt, VARARGS);
    return r;
    #else
    void* f = vsnprintf;
    int r = ((iFpupp_t)f)(buff, s, fmt, *(uint32_t**)b);
    return r;
    #endif
}

EXPORT void* my2_SDL_CreateThread(x86emu_t* emu, void* f, void* n, void* p)
{
    sdl2_my_t *my = (sdl2_my_t *)emu->context->sdl2lib->priv.w.p2;

    void* et = NULL;
    return my->SDL_CreateThread(my_prepare_thread(emu, f, p, 0, &et), n, et);
}

EXPORT int my2_SDL_snprintf(x86emu_t* emu, void* buff, uint32_t s, void * fmt, void * b, va_list V) {
    #ifndef NOALIGN
    // need to align on arm
    myStackAlign((const char*)fmt, b, emu->scratch);
    PREPARE_VALIST;
    void* f = vsnprintf;
    return ((iFpupp_t)f)(buff, s, fmt, VARARGS);
    #else
    return vsnprintf((char*)buff, s, (char*)fmt, V);
    #endif
}

char EXPORT *my2_SDL_GetBasePath(x86emu_t* emu) {
    char* p = strdup(emu->context->fullpath);
    char* b = strrchr(p, '/');
    if(b)
        *(b+1) = '\0';
    return p;
}

EXPORT void my2_SDL_LogCritical(x86emu_t* emu, int32_t cat, void* fmt, void *b) {
    sdl2_my_t *my = (sdl2_my_t *)emu->context->sdl2lib->priv.w.p2;
    // SDL_LOG_PRIORITY_CRITICAL == 6
    #ifndef NOALIGN
    myStackAlign((const char*)fmt, b, emu->scratch);
    PREPARE_VALIST;
    my->SDL_LogMessageV(cat, 6, fmt, VARARGS);
    #else
    my->SDL_LogMessageV(cat, 6, fmt, b);
    #endif
}

EXPORT void my2_SDL_LogError(x86emu_t* emu, int32_t cat, void* fmt, void *b) {
    sdl2_my_t *my = (sdl2_my_t *)emu->context->sdl2lib->priv.w.p2;
    // SDL_LOG_PRIORITY_ERROR == 5
    #ifndef NOALIGN
    myStackAlign((const char*)fmt, b, emu->scratch);
    PREPARE_VALIST;
    my->SDL_LogMessageV(cat, 5, fmt, VARARGS);
    #else
    my->SDL_LogMessageV(cat, 5, fmt, b);
    #endif
}

EXPORT void my2_SDL_LogWarn(x86emu_t* emu, int32_t cat, void* fmt, void *b) {
    sdl2_my_t *my = (sdl2_my_t *)emu->context->sdl2lib->priv.w.p2;
    // SDL_LOG_PRIORITY_WARN == 4
    #ifndef NOALIGN
    myStackAlign((const char*)fmt, b, emu->scratch);
    PREPARE_VALIST;
    my->SDL_LogMessageV(cat, 4, fmt, VARARGS);
    #else
    my->SDL_LogMessageV(cat, 4, fmt, b);
    #endif
}

EXPORT void my2_SDL_LogInfo(x86emu_t* emu, int32_t cat, void* fmt, void *b) {
    sdl2_my_t *my = (sdl2_my_t *)emu->context->sdl2lib->priv.w.p2;
    // SDL_LOG_PRIORITY_INFO == 3
    #ifndef NOALIGN
    myStackAlign((const char*)fmt, b, emu->scratch);
    PREPARE_VALIST;
    my->SDL_LogMessageV(cat, 3, fmt, VARARGS);
    #else
    my->SDL_LogMessageV(cat, 3, fmt, b);
    #endif
}

EXPORT void my2_SDL_LogDebug(x86emu_t* emu, int32_t cat, void* fmt, void *b) {
    sdl2_my_t *my = (sdl2_my_t *)emu->context->sdl2lib->priv.w.p2;
    // SDL_LOG_PRIORITY_DEBUG == 2
    #ifndef NOALIGN
    myStackAlign((const char*)fmt, b, emu->scratch);
    PREPARE_VALIST;
    my->SDL_LogMessageV(cat, 2, fmt, VARARGS);
    #else
    my->SDL_LogMessageV(cat, 2, fmt, b);
    #endif
}

EXPORT void my2_SDL_LogVerbose(x86emu_t* emu, int32_t cat, void* fmt, void *b) {
    sdl2_my_t *my = (sdl2_my_t *)emu->context->sdl2lib->priv.w.p2;
    // SDL_LOG_PRIORITY_VERBOSE == 1
    #ifndef NOALIGN
    myStackAlign((const char*)fmt, b, emu->scratch);
    PREPARE_VALIST;
    my->SDL_LogMessageV(cat, 1, fmt, VARARGS);
    #else
    my->SDL_LogMessageV(cat, 1, fmt, b);
    #endif
}

EXPORT void my2_SDL_Log(x86emu_t* emu, void* fmt, void *b) {
    sdl2_my_t *my = (sdl2_my_t *)emu->context->sdl2lib->priv.w.p2;
    // SDL_LOG_PRIORITY_INFO == 3
    // SDL_LOG_CATEGORY_APPLICATION == 0
    #ifndef NOALIGN
    myStackAlign((const char*)fmt, b, emu->scratch);
    PREPARE_VALIST;
    my->SDL_LogMessageV(0, 3, fmt, VARARGS);
    #else
    my->SDL_LogMessageV(0, 3, fmt, b);
    #endif
}

void fillGLProcWrapper(box86context_t*);
EXPORT void* my2_SDL_GL_GetProcAddress(x86emu_t* emu, void* name) 
{
    khint_t k;
    const char* rname = (const char*)name;
    printf_log(LOG_DEBUG, "Calling SDL_GL_GetProcAddress(%s)\n", rname);
    sdl2_my_t *my = (sdl2_my_t *)emu->context->sdl2lib->priv.w.p2;
    // check if glxprocaddress is filled, and search for lib and fill it if needed
    if(!emu->context->glwrappers)
        fillGLProcWrapper(emu->context);
    // get proc adress using actual glXGetProcAddress
    k = kh_get(symbolmap, emu->context->glmymap, rname);
    int is_my = (k==kh_end(emu->context->glmymap))?0:1;
    void* symbol;
    if(is_my) {
        // try again, by using custom "my_" now...
        char tmp[200];
        strcpy(tmp, "my_");
        strcat(tmp, rname);
        symbol = dlsym(emu->context->box86lib, tmp);
    } else 
        symbol = my->SDL_GL_GetProcAddress(name);
    if(!symbol)
        return NULL;    // easy
    // check if alread bridged
    uintptr_t ret = CheckBridged(emu->context->system, symbol);
    if(ret)
        return (void*)ret; // already bridged
    // get wrapper    
    k = kh_get(symbolmap, emu->context->glwrappers, rname);
    if(k==kh_end(emu->context->glwrappers) && strstr(rname, "ARB")==NULL) {
        // try again, adding ARB at the end if not present
        char tmp[200];
        strcpy(tmp, rname);
        strcat(tmp, "ARB");
        k = kh_get(symbolmap, emu->context->glwrappers, tmp);
    }
    if(k==kh_end(emu->context->glwrappers) && strstr(rname, "EXT")==NULL) {
        // try again, adding EXT at the end if not present
        char tmp[200];
        strcpy(tmp, rname);
        strcat(tmp, "EXT");
        k = kh_get(symbolmap, emu->context->glwrappers, tmp);
    }
    if(k==kh_end(emu->context->glwrappers)) {
        printf_log(LOG_INFO, "Warning, no wrapper for %s\n", rname);
        return NULL;
    }
    AddOffsetSymbol(emu->context->maplib, symbol, rname);
    return (void*)AddBridge(emu->context->system, kh_value(emu->context->glwrappers, k), symbol, 0);
}

#define nb_once	16
typedef void(*sdl2_tls_dtor)(void*);
static uintptr_t dtor_emu[nb_once] = {0};
static void tls_dtor_callback(int n, void* a)
{
	if(dtor_emu[n]) {
        RunFunction(my_context, dtor_emu[n], 1, a);
	}
}
#define GO(N) \
void tls_dtor_callback_##N(void* a) \
{ \
	tls_dtor_callback(N, a); \
}

GO(0)
GO(1)
GO(2)
GO(3)
GO(4)
GO(5)
GO(6)
GO(7)
GO(8)
GO(9)
GO(10)
GO(11)
GO(12)
GO(13)
GO(14)
GO(15)
#undef GO
static const sdl2_tls_dtor dtor_cb[nb_once] = {
	 tls_dtor_callback_0, tls_dtor_callback_1, tls_dtor_callback_2, tls_dtor_callback_3
	,tls_dtor_callback_4, tls_dtor_callback_5, tls_dtor_callback_6, tls_dtor_callback_7
	,tls_dtor_callback_8, tls_dtor_callback_9, tls_dtor_callback_10,tls_dtor_callback_11
	,tls_dtor_callback_12,tls_dtor_callback_13,tls_dtor_callback_14,tls_dtor_callback_15
};
EXPORT int32_t my2_SDL_TLSSet(x86emu_t* emu, uint32_t id, void* value, void* dtor)
{
    sdl2_my_t *my = (sdl2_my_t *)emu->context->sdl2lib->priv.w.p2;

	if(!dtor)
		return my->SDL_TLSSet(id, value, NULL);
	int n = 0;
	while (n<nb_once) {
		if(!dtor_emu[n] || (dtor_emu[n])==((uintptr_t)dtor)) {
			dtor_emu[n] = (uintptr_t)dtor;
			return my->SDL_TLSSet(id, value, dtor_cb[n]);
		}
		++n;
	}
	printf_log(LOG_NONE, "Error: SDL2 SDL_TLSSet with destructor: no more slot!\n");
	//emu->quit = 1;
	return -1;
}

EXPORT void* my2_SDL_JoystickGetDeviceGUID(x86emu_t* emu, void* p, int32_t idx)
{
    sdl2_my_t *my = (sdl2_my_t *)emu->context->sdl2lib->priv.w.p2;
    *(SDL_JoystickGUID*)p = my->SDL_JoystickGetDeviceGUID(idx);
    return p;
}

EXPORT void* my2_SDL_JoystickGetGUID(x86emu_t* emu, void* p, void* joystick)
{
    sdl2_my_t *my = (sdl2_my_t *)emu->context->sdl2lib->priv.w.p2;
    *(SDL_JoystickGUID*)p = my->SDL_JoystickGetGUID(joystick);
    return p;
}

EXPORT void* my2_SDL_JoystickGetGUIDFromString(x86emu_t* emu, void* p, void* pchGUID)
{
    sdl2_my_t *my = (sdl2_my_t *)emu->context->sdl2lib->priv.w.p2;
    *(SDL_JoystickGUID*)p = my->SDL_JoystickGetGUIDFromString(pchGUID);
    return p;
}

EXPORT void* my2_SDL_GameControllerGetBindForAxis(x86emu_t* emu, void* p, void* controller, int32_t axis)
{
    sdl2_my_t *my = (sdl2_my_t *)emu->context->sdl2lib->priv.w.p2;
    *(SDL_GameControllerButtonBind*)p = my->SDL_GameControllerGetBindForAxis(controller, axis);
    return p;
}

EXPORT void* my2_SDL_GameControllerGetBindForButton(x86emu_t* emu, void* p, void* controller, int32_t button)
{
    sdl2_my_t *my = (sdl2_my_t *)emu->context->sdl2lib->priv.w.p2;
    *(SDL_GameControllerButtonBind*)p = my->SDL_GameControllerGetBindForButton(controller, button);
    return p;
}

EXPORT void* my2_SDL_GameControllerMappingForGUID(x86emu_t* emu, void* p)
{
    sdl2_my_t *my = (sdl2_my_t *)emu->context->sdl2lib->priv.w.p2;
    return my->SDL_GameControllerMappingForGUID(*(SDL_JoystickGUID*)p);
}

EXPORT void my2_SDL_AddEventWatch(x86emu_t* emu, void* p, void* userdata)
{
    sdl2_my_t *my = (sdl2_my_t *)emu->context->sdl2lib->priv.w.p2;
    my->SDL_AddEventWatch(find_eventfilter_Fct(p), userdata);
}
EXPORT void my2_SDL_DelEventWatch(x86emu_t* emu, void* p, void* userdata)
{
    sdl2_my_t *my = (sdl2_my_t *)emu->context->sdl2lib->priv.w.p2;
    my->SDL_DelEventWatch(find_eventfilter_Fct(p), userdata);
}

// DL functions from wrappedlibdl.c
void* my_dlopen(x86emu_t* emu, void *filename, int flag);
int my_dlclose(x86emu_t* emu, void *handle);
void* my_dlsym(x86emu_t* emu, void *handle, void *symbol);
EXPORT void* my2_SDL_LoadObject(x86emu_t* emu, void* sofile)
{
    return my_dlopen(emu, sofile, 0);   // TODO: check correct flag value...
}
EXPORT void my2_SDL_UnloadObject(x86emu_t* emu, void* handle)
{
    my_dlclose(emu, handle);
}
EXPORT void* my2_SDL_LoadFunction(x86emu_t* emu, void* handle, void* name)
{
    return my_dlsym(emu, handle, name);
}

EXPORT void my2_SDL_GetJoystickGUIDInfo(x86emu_t* emu, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint16_t* vendor, uint16_t* product, uint16_t* version)
{
    sdl2_my_t *my = (sdl2_my_t *)emu->context->sdl2lib->priv.w.p2;

    if(my->SDL_GetJoystickGUIDInfo) {
        SDL_JoystickGUID_Helper guid;
        guid.u[0] = a;
        guid.u[1] = b;
        guid.u[2] = c;
        guid.u[3] = d;
        my->SDL_GetJoystickGUIDInfo(guid.guid, vendor, product, version);
    } else {
        // dummy, set everything to "unknown"
        if(vendor)  *vendor = 0;
        if(product) *product = 0;
        if(version) *version = 0;
    }
}

EXPORT int32_t my2_SDL_IsJoystickPS4(x86emu_t* emu, uint16_t vendor, uint16_t product_id)
{
    sdl2_my_t *my = (sdl2_my_t *)emu->context->sdl2lib->priv.w.p2;

    if(my->SDL_IsJoystickPS4)
        return my->SDL_IsJoystickPS4(vendor, product_id);
    // fallback
    return 0;
}
EXPORT int32_t my2_SDL_IsJoystickNintendoSwitchPro(x86emu_t* emu, uint16_t vendor, uint16_t product_id)
{
    sdl2_my_t *my = (sdl2_my_t *)emu->context->sdl2lib->priv.w.p2;

    if(my->SDL_IsJoystickNintendoSwitchPro)
        return my->SDL_IsJoystickNintendoSwitchPro(vendor, product_id);
    // fallback
    return 0;
}
EXPORT int32_t my2_SDL_IsJoystickSteamController(x86emu_t* emu, uint16_t vendor, uint16_t product_id)
{
    sdl2_my_t *my = (sdl2_my_t *)emu->context->sdl2lib->priv.w.p2;

    if(my->SDL_IsJoystickSteamController)
        return my->SDL_IsJoystickSteamController(vendor, product_id);
    // fallback
    return 0;
}
EXPORT int32_t my2_SDL_IsJoystickXbox360(x86emu_t* emu, uint16_t vendor, uint16_t product_id)
{
    sdl2_my_t *my = (sdl2_my_t *)emu->context->sdl2lib->priv.w.p2;

    if(my->SDL_IsJoystickXbox360)
        return my->SDL_IsJoystickXbox360(vendor, product_id);
    // fallback
    return 0;
}
EXPORT int32_t my2_SDL_IsJoystickXboxOne(x86emu_t* emu, uint16_t vendor, uint16_t product_id)
{
    sdl2_my_t *my = (sdl2_my_t *)emu->context->sdl2lib->priv.w.p2;

    if(my->SDL_IsJoystickXboxOne)
        return my->SDL_IsJoystickXboxOne(vendor, product_id);
    // fallback
    return 0;
}
EXPORT int32_t my2_SDL_IsJoystickXInput(x86emu_t* emu, void *p)
{
    sdl2_my_t *my = (sdl2_my_t *)emu->context->sdl2lib->priv.w.p2;

    if(my->SDL_IsJoystickXInput)
        return my->SDL_IsJoystickXInput(*(SDL_JoystickGUID*)p);
    // fallback
    return 0;
}
EXPORT int32_t my2_SDL_IsJoystickHIDAPI(x86emu_t* emu, void *p)
{
    sdl2_my_t *my = (sdl2_my_t *)emu->context->sdl2lib->priv.w.p2;

    if(my->SDL_IsJoystickHIDAPI)
        return my->SDL_IsJoystickHIDAPI(*(SDL_JoystickGUID*)p);
    // fallback
    return 0;
}

void* my_vkGetInstanceProcAddr(x86emu_t* emu, void* device, void* name);
EXPORT void* my2_SDL_Vulkan_GetVkGetInstanceProcAddr(x86emu_t* emu)
{
    sdl2_my_t *my = (sdl2_my_t *)emu->context->sdl2lib->priv.w.p2;
    
    if(!emu->context->vkprocaddress)
        emu->context->vkprocaddress = (vkprocaddess_t)my->SDL_Vulkan_GetVkGetInstanceProcAddr();

    if(emu->context->vkprocaddress)
        return (void*)AddCheckBridge(my_context->sdl2lib->priv.w.bridge, pFEpp, my_vkGetInstanceProcAddr, 0);
    return NULL;
}

const char* sdl2Name = "libSDL2-2.0.so.0";
#define LIBNAME sdl2

#define CUSTOM_INIT \
    box86->sdl2lib = lib;                                           \
    lib->priv.w.p2 = getSDL2My(lib);                                \
    box86->sdl2allocrw = ((sdl2_my_t*)lib->priv.w.p2)->SDL_AllocRW; \
    box86->sdl2freerw  = ((sdl2_my_t*)lib->priv.w.p2)->SDL_FreeRW;  \
    lib->altmy = strdup("my2_");                                    \
    lib->priv.w.needed = 4;                                         \
    lib->priv.w.neededlibs = (char**)calloc(lib->priv.w.needed, sizeof(char*)); \
    lib->priv.w.neededlibs[0] = strdup("libdl.so.2");               \
    lib->priv.w.neededlibs[1] = strdup("libm.so.6");                \
    lib->priv.w.neededlibs[2] = strdup("librt.so.1");               \
    lib->priv.w.neededlibs[3] = strdup("libpthread.so.0");

#define CUSTOM_FINI \
    ((sdl2_my_t *)lib->priv.w.p2)->SDL_Quit();              \
    freeSDL2My(lib->priv.w.p2);                             \
    free(lib->priv.w.p2);                                   \
    ((box86context_t*)(lib->context))->sdl2lib = NULL;      \
    ((box86context_t*)(lib->context))->sdl2allocrw = NULL;  \
    ((box86context_t*)(lib->context))->sdl2freerw = NULL;


#include "wrappedlib_init.h"


