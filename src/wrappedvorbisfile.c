#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define _GNU_SOURCE         /* See feature_test_macros(7) */
#include <dlfcn.h>

#include "wrappedlibs.h"

#include "debug.h"
#include "wrapper.h"
#include "bridge.h"
#include "library_private.h"
#include "x86emu.h"
#include "x86emu_private.h"
#include "callback.h"
#include "box86context.h"
#include "librarian.h"
#include "myalign.h"

const char* vorbisfileName = "libvorbisfile.so.3";
#define LIBNAME vorbisfile


typedef void*   (*pFpi_t)(void*, int32_t);
typedef int32_t (*iFp_t)(void*);
typedef int32_t (*iFpi_t)(void*, int32_t);
typedef int32_t (*iFpp_t)(void*, void*);
typedef int32_t (*iFpd_t)(void*, double);
typedef int32_t (*iFpI_t)(void*, int64_t);
typedef int32_t (*iFppip_t)(void*, void*, int32_t, void*);
typedef int32_t (*iFpppi_t)(void*, void*, void*, int32_t);
typedef int32_t (*iFpppi_t)(void*, void*, void*, int32_t);
typedef int32_t (*iFpppiC_t)(void*, void*, void*, int32_t, ov_callbacks);
typedef int32_t (*iFppiiiip_t)(void*, void*, int32_t, int32_t, int32_t, int32_t, void*);
typedef int64_t (*IFp_t)(void*);
typedef int64_t (*IFpi_t)(void*, int32_t);
typedef double  (*dFp_t)(void*);
typedef double  (*dFpi_t)(void*, int32_t);

typedef struct vorbisfile_my_s {
    // functions
    iFpi_t          ov_bitrate;
    iFp_t           ov_bitrate_instant;
    iFp_t           ov_clear;
    iFpi_t          ov_comment;
    iFpp_t          ov_crosslap;
    iFpp_t          ov_fopen;
    iFpi_t          ov_halfrate;
    iFp_t           ov_halfrate_p;
    pFpi_t          ov_info;
    iFpppi_t        ov_open;
    iFpppiC_t       ov_open_callbacks;
    iFpI_t          ov_pcm_seek;
    iFpI_t          ov_pcm_seek_lap;
    iFpI_t          ov_pcm_seek_page;
    iFpI_t          ov_pcm_seek_page_lap;
    IFp_t           ov_pcm_tell;
    IFpi_t          ov_pcm_total;
    iFpi_t          ov_raw_seek;
    iFpi_t          ov_raw_seek_lap;
    IFp_t           ov_raw_tell;
    IFpi_t          ov_raw_total;
    iFppiiiip_t     ov_read;
    iFppip_t        ov_read_float;
    iFp_t           ov_seekable;
    iFpi_t          ov_serialnumber;
    iFp_t           ov_streams;
    iFpppi_t        ov_test;
    iFp_t           ov_test_open;
    iFpd_t          ov_time_seek;
    iFpd_t          ov_time_seek_lap;
    iFpd_t          ov_time_seek_page;
    iFpd_t          ov_time_seek_page_lap;
    dFp_t           ov_time_tell;
    dFpi_t          ov_time_total;
} vorbisfile_my_t;

void* getVorbisfileMy(library_t* lib)
{
    vorbisfile_my_t* my = (vorbisfile_my_t*)calloc(1, sizeof(vorbisfile_my_t));
    #define GO(A, W) my->A = (W)dlsym(lib->priv.w.lib, #A);
    GO(ov_bitrate, iFpi_t)
    GO(ov_bitrate_instant, iFp_t)
    GO(ov_clear, iFp_t)
    GO(ov_comment, iFpi_t)
    GO(ov_crosslap, iFpp_t)
    GO(ov_fopen, iFpp_t)
    GO(ov_halfrate, iFpi_t)
    GO(ov_halfrate_p, iFp_t)
    GO(ov_info, pFpi_t)
    GO(ov_open, iFpppi_t)
    GO(ov_open_callbacks, iFpppiC_t)
    GO(ov_pcm_seek, iFpI_t)
    GO(ov_pcm_seek_lap, iFpI_t)
    GO(ov_pcm_seek_page, iFpI_t)
    GO(ov_pcm_seek_page_lap, iFpI_t)
    GO(ov_pcm_tell, IFp_t)
    GO(ov_pcm_total, IFpi_t)
    GO(ov_raw_seek, iFpi_t)
    GO(ov_raw_seek_lap, iFpi_t)
    GO(ov_raw_tell, IFp_t)
    GO(ov_raw_total, IFpi_t)
    GO(ov_read, iFppiiiip_t)
    GO(ov_seekable, iFp_t)
    GO(ov_serialnumber, iFpi_t)
    GO(ov_streams, iFp_t)
    GO(ov_test, iFpppi_t)
    GO(ov_test_open, iFp_t)
    GO(ov_time_seek, iFpd_t)
    GO(ov_time_seek_lap, iFpd_t)
    GO(ov_time_seek_page, iFpd_t)
    GO(ov_time_seek_page_lap, iFpd_t)
    GO(ov_time_tell, dFp_t)
    GO(ov_time_total, dFpi_t)
    #undef GO
    return my;
}

void freeVorbisfileMy(void* lib)
{
    vorbisfile_my_t *my = (vorbisfile_my_t *)lib;
}

int32_t my_ov_open_callbacks(x86emu_t* emu, void* datasource, void* vf, void* initial, int32_t ibytes, void* read, void* seek, void* close, void* tell);

#ifndef NOALIGN

EXPORT int32_t my_ov_bitrate(x86emu_t* emu, void* vf, int32_t i) {
    vorbisfile_my_t* my = (vorbisfile_my_t*)emu->context->vorbisfile->priv.w.p2;
    OggVorbis oggvorbis;
    AlignOggVorbis(&oggvorbis, vf);
    return my->ov_bitrate(&oggvorbis, i);
}
EXPORT int32_t my_ov_bitrate_instant(x86emu_t* emu, void* vf) {
    vorbisfile_my_t* my = (vorbisfile_my_t*)emu->context->vorbisfile->priv.w.p2;
    OggVorbis oggvorbis;
    AlignOggVorbis(&oggvorbis, vf);
    int32_t ret = my->ov_bitrate_instant(&oggvorbis);
    UnalignOggVorbis(vf, &oggvorbis);
    return ret;
}
EXPORT int32_t my_ov_clear(x86emu_t* emu, void* vf) {
    vorbisfile_my_t* my = (vorbisfile_my_t*)emu->context->vorbisfile->priv.w.p2;
    OggVorbis oggvorbis;
    AlignOggVorbis(&oggvorbis, vf);
    int32_t ret = my->ov_clear(&oggvorbis);
    UnalignOggVorbis(vf, &oggvorbis);
    return ret;
}
EXPORT int32_t my_ov_comment(x86emu_t* emu, void* vf, int32_t i) {
    vorbisfile_my_t* my = (vorbisfile_my_t*)emu->context->vorbisfile->priv.w.p2;
    OggVorbis oggvorbis;
    AlignOggVorbis(&oggvorbis, vf);
    return my->ov_comment(&oggvorbis, i);
}
EXPORT int32_t my_ov_crosslap(x86emu_t* emu, void* vf, void* v2) {
    vorbisfile_my_t* my = (vorbisfile_my_t*)emu->context->vorbisfile->priv.w.p2;
    OggVorbis oggvorbis, ov2;
    AlignOggVorbis(&oggvorbis, vf);
    AlignOggVorbis(&ov2, v2);
    int32_t ret = my->ov_crosslap(&oggvorbis, &ov2);
    UnalignOggVorbis(vf, &oggvorbis);
    UnalignOggVorbis(v2, &ov2);
    return ret;
}
EXPORT int32_t my_ov_fopen(x86emu_t* emu, void* p, void* vf) {
    vorbisfile_my_t* my = (vorbisfile_my_t*)emu->context->vorbisfile->priv.w.p2;
    OggVorbis oggvorbis;
    AlignOggVorbis(&oggvorbis, vf);
    int32_t ret = my->ov_fopen(p, &oggvorbis);
    UnalignOggVorbis(vf, &oggvorbis);
    return ret;
}
EXPORT int32_t my_ov_halfrate(x86emu_t* emu, void* vf, int32_t i) {
    vorbisfile_my_t* my = (vorbisfile_my_t*)emu->context->vorbisfile->priv.w.p2;
    OggVorbis oggvorbis;
    AlignOggVorbis(&oggvorbis, vf);
    int32_t ret = my->ov_halfrate(&oggvorbis, i);
    UnalignOggVorbis(vf, &oggvorbis);
    return ret;
}
EXPORT int32_t my_ov_halfrate_p(x86emu_t* emu, void* vf) {
    vorbisfile_my_t* my = (vorbisfile_my_t*)emu->context->vorbisfile->priv.w.p2;
    OggVorbis oggvorbis;
    AlignOggVorbis(&oggvorbis, vf);
    int32_t ret = my->ov_halfrate_p(&oggvorbis);
    UnalignOggVorbis(vf, &oggvorbis);
    return ret;
}
EXPORT void* my_ov_info(x86emu_t* emu, void* vf, int32_t i) {
    vorbisfile_my_t* my = (vorbisfile_my_t*)emu->context->vorbisfile->priv.w.p2;
    OggVorbis oggvorbis;
    AlignOggVorbis(&oggvorbis, vf);
    void* ret = my->ov_info(&oggvorbis, i);
    UnalignOggVorbis(vf, &oggvorbis);
    return ret;
}
EXPORT int32_t my_ov_open(x86emu_t* emu, void* f, void* vf, void* init, int32_t i) {
    vorbisfile_my_t* my = (vorbisfile_my_t*)emu->context->vorbisfile->priv.w.p2;
    OggVorbis oggvorbis;
    AlignOggVorbis(&oggvorbis, vf);
    int32_t ret = my->ov_open(f, &oggvorbis, init, i);
    UnalignOggVorbis(vf, &oggvorbis);
    return ret;
}
EXPORT int32_t my_ov_pcm_seek(x86emu_t* emu, void* vf, int64_t i) {
    vorbisfile_my_t* my = (vorbisfile_my_t*)emu->context->vorbisfile->priv.w.p2;
    OggVorbis oggvorbis;
    AlignOggVorbis(&oggvorbis, vf);
    int32_t ret = my->ov_pcm_seek(&oggvorbis, i);
    UnalignOggVorbis(vf, &oggvorbis);
    return ret;
}
EXPORT int32_t my_ov_pcm_seek_lap(x86emu_t* emu, void* vf, int64_t i) {
    vorbisfile_my_t* my = (vorbisfile_my_t*)emu->context->vorbisfile->priv.w.p2;
    OggVorbis oggvorbis;
    AlignOggVorbis(&oggvorbis, vf);
    int32_t ret = my->ov_pcm_seek_lap(&oggvorbis, i);
    UnalignOggVorbis(vf, &oggvorbis);
    return ret;
}
EXPORT int32_t my_ov_pcm_seek_page(x86emu_t* emu, void* vf, int64_t i) {
    vorbisfile_my_t* my = (vorbisfile_my_t*)emu->context->vorbisfile->priv.w.p2;
    OggVorbis oggvorbis;
    AlignOggVorbis(&oggvorbis, vf);
    int32_t ret = my->ov_pcm_seek_page(&oggvorbis, i);
    UnalignOggVorbis(vf, &oggvorbis);
    return ret;
}
EXPORT int32_t my_ov_pcm_seek_page_lap(x86emu_t* emu, void* vf, int64_t i) {
    vorbisfile_my_t* my = (vorbisfile_my_t*)emu->context->vorbisfile->priv.w.p2;
    OggVorbis oggvorbis;
    AlignOggVorbis(&oggvorbis, vf);
    int32_t ret = my->ov_pcm_seek_page_lap(&oggvorbis, i);
    UnalignOggVorbis(vf, &oggvorbis);
    return ret;
}
EXPORT int64_t my_ov_pcm_tell(x86emu_t* emu, void* vf) {
    vorbisfile_my_t* my = (vorbisfile_my_t*)emu->context->vorbisfile->priv.w.p2;
    OggVorbis oggvorbis;
    AlignOggVorbis(&oggvorbis, vf);
    int64_t ret = my->ov_pcm_tell(&oggvorbis);
    UnalignOggVorbis(vf, &oggvorbis);
    return ret;
}
EXPORT int64_t my_ov_pcm_total(x86emu_t* emu, void* vf, int32_t i) {
    vorbisfile_my_t* my = (vorbisfile_my_t*)emu->context->vorbisfile->priv.w.p2;
    OggVorbis oggvorbis;
    AlignOggVorbis(&oggvorbis, vf);
    int64_t ret = my->ov_pcm_total(&oggvorbis, i);
    UnalignOggVorbis(vf, &oggvorbis);
    return ret;
}
EXPORT int32_t my_ov_raw_seek(x86emu_t* emu, void* vf, int32_t i) {
    vorbisfile_my_t* my = (vorbisfile_my_t*)emu->context->vorbisfile->priv.w.p2;
    OggVorbis oggvorbis;
    AlignOggVorbis(&oggvorbis, vf);
    int32_t ret = my->ov_raw_seek(&oggvorbis, i);
    UnalignOggVorbis(vf, &oggvorbis);
    return ret;
}
EXPORT int32_t my_ov_raw_seek_lap(x86emu_t* emu, void* vf, int32_t i) {
    vorbisfile_my_t* my = (vorbisfile_my_t*)emu->context->vorbisfile->priv.w.p2;
    OggVorbis oggvorbis;
    AlignOggVorbis(&oggvorbis, vf);
    int32_t ret = my->ov_raw_seek_lap(&oggvorbis, i);
    UnalignOggVorbis(vf, &oggvorbis);
    return ret;
}
EXPORT int64_t my_ov_raw_tell(x86emu_t* emu, void* vf) {
    vorbisfile_my_t* my = (vorbisfile_my_t*)emu->context->vorbisfile->priv.w.p2;
    OggVorbis oggvorbis;
    AlignOggVorbis(&oggvorbis, vf);
    int64_t ret = my->ov_raw_tell(&oggvorbis);
    UnalignOggVorbis(vf, &oggvorbis);
    return ret;
}
EXPORT int64_t my_ov_raw_total(x86emu_t* emu, void* vf, int32_t i) {
    vorbisfile_my_t* my = (vorbisfile_my_t*)emu->context->vorbisfile->priv.w.p2;
    OggVorbis oggvorbis;
    AlignOggVorbis(&oggvorbis, vf);
    int64_t ret = my->ov_raw_total(&oggvorbis, i);
    UnalignOggVorbis(vf, &oggvorbis);
    return ret;
}
EXPORT int32_t my_ov_read(x86emu_t* emu, void* vf, void* buff, int32_t l, int32_t b, int32_t w, int32_t s, void* bs) {
    vorbisfile_my_t* my = (vorbisfile_my_t*)emu->context->vorbisfile->priv.w.p2;
    OggVorbis oggvorbis;
    AlignOggVorbis(&oggvorbis, vf);
    int32_t ret = my->ov_read(&oggvorbis, buff, l, b, w, s, bs);
    UnalignOggVorbis(vf, &oggvorbis);
    return ret;
}
EXPORT int32_t my_ov_read_float(x86emu_t* emu, void* vf, void* buff, int32_t l, void* bs) {
    vorbisfile_my_t* my = (vorbisfile_my_t*)emu->context->vorbisfile->priv.w.p2;
    OggVorbis oggvorbis;
    AlignOggVorbis(&oggvorbis, vf);
    int32_t ret = my->ov_read_float(&oggvorbis, buff, l, bs);
    UnalignOggVorbis(vf, &oggvorbis);
    return ret;
}
EXPORT int32_t my_ov_seekable(x86emu_t* emu, void* vf) {
    vorbisfile_my_t* my = (vorbisfile_my_t*)emu->context->vorbisfile->priv.w.p2;
    OggVorbis oggvorbis;
    AlignOggVorbis(&oggvorbis, vf);
    int32_t ret = my->ov_seekable(&oggvorbis);
    UnalignOggVorbis(vf, &oggvorbis);
    return ret;
}
EXPORT int32_t my_ov_serialnumber(x86emu_t* emu, void* vf, int32_t i) {
    vorbisfile_my_t* my = (vorbisfile_my_t*)emu->context->vorbisfile->priv.w.p2;
    OggVorbis oggvorbis;
    AlignOggVorbis(&oggvorbis, vf);
    int32_t ret = my->ov_serialnumber(&oggvorbis, i);
    UnalignOggVorbis(vf, &oggvorbis);
    return ret;
}
EXPORT int32_t my_ov_streams(x86emu_t* emu, void* vf) {
    vorbisfile_my_t* my = (vorbisfile_my_t*)emu->context->vorbisfile->priv.w.p2;
    OggVorbis oggvorbis;
    AlignOggVorbis(&oggvorbis, vf);
    int32_t ret = my->ov_streams(&oggvorbis);
    UnalignOggVorbis(vf, &oggvorbis);
    return ret;
}
EXPORT int32_t my_ov_test(x86emu_t* emu, void* p, void* vf, void* bs, int32_t l) {
    vorbisfile_my_t* my = (vorbisfile_my_t*)emu->context->vorbisfile->priv.w.p2;
    OggVorbis oggvorbis;
    AlignOggVorbis(&oggvorbis, vf);
    int32_t ret = my->ov_test(p, &oggvorbis, bs, l);
    UnalignOggVorbis(vf, &oggvorbis);
    return ret;
}
EXPORT int32_t my_ov_test_open(x86emu_t* emu, void* vf) {
    vorbisfile_my_t* my = (vorbisfile_my_t*)emu->context->vorbisfile->priv.w.p2;
    OggVorbis oggvorbis;
    AlignOggVorbis(&oggvorbis, vf);
    int32_t ret = my->ov_test_open(&oggvorbis);
    UnalignOggVorbis(vf, &oggvorbis);
    return ret;
}
EXPORT int32_t my_ov_time_seek(x86emu_t* emu, void* vf, double d) {
    vorbisfile_my_t* my = (vorbisfile_my_t*)emu->context->vorbisfile->priv.w.p2;
    OggVorbis oggvorbis;
    AlignOggVorbis(&oggvorbis, vf);
    int32_t ret = my->ov_time_seek(&oggvorbis, d);
    UnalignOggVorbis(vf, &oggvorbis);
    return ret;
}
EXPORT int32_t my_ov_time_seek_lap(x86emu_t* emu, void* vf, double d) {
    vorbisfile_my_t* my = (vorbisfile_my_t*)emu->context->vorbisfile->priv.w.p2;
    OggVorbis oggvorbis;
    AlignOggVorbis(&oggvorbis, vf);
    int32_t ret = my->ov_time_seek_lap(&oggvorbis, d);
    UnalignOggVorbis(vf, &oggvorbis);
    return ret;
}
EXPORT int32_t my_ov_time_seek_page(x86emu_t* emu, void* vf, double d) {
    vorbisfile_my_t* my = (vorbisfile_my_t*)emu->context->vorbisfile->priv.w.p2;
    OggVorbis oggvorbis;
    AlignOggVorbis(&oggvorbis, vf);
    int32_t ret = my->ov_time_seek_page(&oggvorbis, d);
    UnalignOggVorbis(vf, &oggvorbis);
    return ret;
}
EXPORT int32_t my_ov_time_seek_page_lap(x86emu_t* emu, void* vf, double d) {
    vorbisfile_my_t* my = (vorbisfile_my_t*)emu->context->vorbisfile->priv.w.p2;
    OggVorbis oggvorbis;
    AlignOggVorbis(&oggvorbis, vf);
    int32_t ret = my->ov_time_seek_page_lap(&oggvorbis, d);
    UnalignOggVorbis(vf, &oggvorbis);
    return ret;
}
EXPORT double my_ov_time_tell(x86emu_t* emu, void* vf) {
    vorbisfile_my_t* my = (vorbisfile_my_t*)emu->context->vorbisfile->priv.w.p2;
    OggVorbis oggvorbis;
    AlignOggVorbis(&oggvorbis, vf);
    double ret = my->ov_time_tell(&oggvorbis);
    UnalignOggVorbis(vf, &oggvorbis);
    return ret;
}
EXPORT double my_ov_time_total(x86emu_t* emu, void* vf, int32_t i) {
    vorbisfile_my_t* my = (vorbisfile_my_t*)emu->context->vorbisfile->priv.w.p2;
    OggVorbis oggvorbis;
    AlignOggVorbis(&oggvorbis, vf);
    double ret = my->ov_time_total(&oggvorbis, i);
    UnalignOggVorbis(vf, &oggvorbis);
    return ret;
}
#endif  //!NOALIGN

#define CUSTOM_INIT \
    box86->vorbisfile = lib; \
    lib->priv.w.p2 = getVorbisfileMy(lib);

#define CUSTOM_FINI \
    freeVorbisfileMy(lib->priv.w.p2); \
    free(lib->priv.w.p2); \
    lib->context->vorbisfile = NULL;

#include "wrappedlib_init.h"

typedef union ui64_s {
    int64_t     i;
    uint64_t    u;
    uint32_t    d[2];
} ui64_t;

static size_t my_read_func(x86emu_t* emu, void *ptr, size_t size, size_t nmemb, void *datasource)
{
    SetCallbackNArg(emu, 4);
    SetCallbackArg(emu, 0, ptr);
    SetCallbackArg(emu, 1, (void*)size);
    SetCallbackArg(emu, 2, (void*)nmemb);
    SetCallbackArg(emu, 3, datasource);
    void* fnc = GetCallbackArg(emu, 5);
    SetCallbackAddress(emu, (uintptr_t)fnc);
    return RunCallback(emu);
}
static int my_seek_func(x86emu_t* emu, void *datasource, int64_t offset, int whence)
{
    SetCallbackNArg(emu, 4);    // because offset is 64bits...
    SetCallbackArg(emu, 0, datasource);
    ui64_t ofs;
    ofs.i = offset;
    SetCallbackArg(emu, 1, (void*)ofs.d[0]);
    SetCallbackArg(emu, 2, (void*)ofs.d[1]);
    SetCallbackArg(emu, 3, (void*)whence);
    void* fnc = GetCallbackArg(emu, 6);
    SetCallbackAddress(emu, (uintptr_t)fnc);
    return RunCallback(emu);
}
static int my_close_func(x86emu_t* emu, void *datasource)
{
    SetCallbackNArg(emu, 1);
    SetCallbackArg(emu, 0, datasource);
    void* fnc = GetCallbackArg(emu, 7);
    int r = 0;
    if(fnc) {
        SetCallbackAddress(emu, (uintptr_t)fnc);
        r = RunCallback(emu);
    }
    FreeCallback(emu);
    return r;
}
static long my_tell_func(x86emu_t* emu, void *datasource)
{
    SetCallbackNArg(emu, 1);
    SetCallbackArg(emu, 0, datasource);
    void* fnc = GetCallbackArg(emu, 8);
    SetCallbackAddress(emu, (uintptr_t)fnc);
    return RunCallback(emu);
}

#define NMAX 8
static x86emu_t *cb_emu[NMAX] = {0};
static int cb_used[NMAX] = {0}; // probably need some mutex?
#define GO(A) \
static size_t my_read_func_##A(void *ptr, size_t size, size_t nmemb, void *datasource) {return my_read_func(cb_emu[A], ptr, size, nmemb, datasource);} \
static int my_seek_func_##A(void *datasource, int64_t offset, int whence) {return my_seek_func(cb_emu[A], datasource, offset, whence);} \
static int my_close_func_##A(void *datasource) { \
int ret = my_close_func(cb_emu[A], datasource); \
cb_used[A] = 0; \
return ret; \
}\
static long my_tell_func_##A(void *datasource) {return my_tell_func(cb_emu[A], datasource);}
GO(0)
GO(1)
GO(2)
GO(3)
GO(4)
GO(5)
GO(6)
GO(7)
GO(8)
#undef GO
#define GO(A) {my_read_func_##A, my_seek_func_##A, my_close_func_##A, my_tell_func_##A},
static ov_callbacks my_ov_callbacks[] = {
GO(0)
GO(1)
GO(2)
GO(3)
GO(4)
GO(5)
GO(6)
GO(7)
GO(8)
};
#undef GO

EXPORT int32_t my_ov_open_callbacks(x86emu_t* emu, void* datasource, void* vf, void* initial, int32_t ibytes, void* read_fnc, void* seek_fnc, void* close_fnc, void* tell_fnc)
{
    vorbisfile_my_t* my = (vorbisfile_my_t*)emu->context->vorbisfile->priv.w.p2;
    // search for free slot
    int slot = 0;
    while(slot<NMAX && cb_used[slot]) ++slot;
    if(slot==NMAX) {
        printf_log(LOG_NONE, "BOX86: Error, no more slot in ov_open_callbacks\n");
        //emu->quit = 1;
        return -1;
    }
    cb_used[slot] = 1;
    // wrap all callbacks, add close if not there to free the callbackemu
    ov_callbacks cbs = {0};
    cb_emu[slot] = AddCallback(emu, (uintptr_t)read_fnc, 4, NULL, NULL, NULL, NULL);
    SetCallbackArg(cb_emu[slot], 5, read_fnc);
    SetCallbackArg(cb_emu[slot], 6, seek_fnc);
    SetCallbackArg(cb_emu[slot], 7, close_fnc);
    SetCallbackArg(cb_emu[slot], 8, tell_fnc);
    cbs.read_func = my_ov_callbacks[slot].read_func;
    if(seek_fnc) cbs.seek_func = my_ov_callbacks[slot].seek_func;
    cbs.close_func = my_ov_callbacks[slot].close_func;
    if(tell_fnc) cbs.tell_func = my_ov_callbacks[slot].tell_func;
    OggVorbis oggvorbis;
    AlignOggVorbis(&oggvorbis, vf);
    int32_t ret =  my->ov_open_callbacks(datasource, &oggvorbis, initial, ibytes, cbs);
    UnalignOggVorbis(vf, &oggvorbis);
    return ret;
}