#include <stdio.h>
#include <stdlib.h>

#include <pthread.h>

#include "debug.h"
#include "librarian.h"
#include "librarian_private.h"
#include "library.h"
#include "library_private.h"
#include "x86emu.h"
#include "box86context.h"
#include "elfloader.h"

#include "bridge.h"

KHASH_MAP_IMPL_STR(mapsymbols, onesymbol_t);
KHASH_MAP_IMPL_INT(mapoffsets, cstr_t);

lib_t *NewLibrarian(box86context_t* context, int ownlibs)
{
    lib_t *maplib = (lib_t*)calloc(1, sizeof(lib_t));
    
    maplib->mapsymbols = kh_init(mapsymbols);
    maplib->weaksymbols = kh_init(mapsymbols);
    maplib->localsymbols = kh_init(mapsymbols);
    maplib->mapoffsets = kh_init(mapoffsets);
    maplib->bridge = NewBridge();

    maplib->context = context;

    maplib->ownlibs = ownlibs;

    return maplib;
}
void FreeLibrarian(lib_t **maplib)
{
    // should that be in reverse order?
    if(!maplib || !*maplib)
        return;
    
    if((*maplib)->ownlibs) {
        printf_log(LOG_DEBUG, "Closing %d libs from maplib %p\n", (*maplib)->libsz, *maplib);
        for (int i=0; i<(*maplib)->libsz; ++i) {
            printf_log(LOG_DEBUG, "Unloading %s\n", (*maplib)->libraries[i].lib->name);
            Free1Library(&(*maplib)->libraries[i].lib);
        }
    }
    free((*maplib)->libraries);
    (*maplib)->libraries = NULL;

    if((*maplib)->mapsymbols) {
        kh_destroy(mapsymbols, (*maplib)->mapsymbols);
    }
    if((*maplib)->weaksymbols) {
        kh_destroy(mapsymbols, (*maplib)->weaksymbols);
    }
    if((*maplib)->localsymbols) {
        kh_destroy(mapsymbols, (*maplib)->localsymbols);
    }
    if((*maplib)->mapoffsets) {
        kh_destroy(mapoffsets, (*maplib)->mapoffsets);
    }
    (*maplib)->libsz = (*maplib)->libcap = 0;

    if((*maplib)->bridge)
        FreeBridge(&(*maplib)->bridge);

    free(*maplib);
    *maplib = NULL;

}

box86context_t* GetLibrarianContext(lib_t* maplib)
{
    return maplib->context;
}

kh_mapsymbols_t* GetMapSymbol(lib_t* maplib)
{
    return maplib->mapsymbols;
}

kh_mapsymbols_t* GetWeakSymbol(lib_t* maplib)
{
    return maplib->weaksymbols;
}

kh_mapsymbols_t* GetLocalSymbol(lib_t* maplib)
{
    return maplib->localsymbols;
}

library_t* getLib(lib_t* maplib, const char* path)
{
    for(int i=0; i<maplib->libsz; ++i) {
        onelib_t *onelib = &maplib->libraries[i];
        if(IsSameLib(onelib->lib, path)) {
            return onelib->lib;
        }
    }
    return NULL;
}

static int libraryInMapLib(lib_t* maplib, library_t* lib)
{
    if(!maplib)
        return 0;
    for(int i=0; i<maplib->libsz; ++i)
        if(maplib->libraries[i].lib==lib)
            return 1;
    return 0;
}

void MapLibAddLib(lib_t* maplib, library_t* lib)
{
    if (maplib->libsz == maplib->libcap) {
        maplib->libcap += 8;
        maplib->libraries = (onelib_t*)realloc(maplib->libraries, maplib->libcap*sizeof(onelib_t));
    }
    maplib->libraries[maplib->libsz].lib = lib;
    maplib->libraries[maplib->libsz].name = GetNameLib(lib);
    ++maplib->libsz;
}

void MapLibAddMapLib(lib_t* dest, lib_t* src)
{
    if(!src)
        return;
    for(int i=0; i<src->libsz; ++i) {
        library_t* lib = src->libraries[i].lib;
        if(!lib) continue;
        if(lib->maplib && src!=lib->maplib) {   //TODO: find why is src!=lib->maplib needed
            MapLibAddMapLib(dest, lib->maplib);
            free(lib->maplib);
            lib->maplib = NULL;
        }
        if(!libraryInMapLib(dest, lib))
            MapLibAddLib(dest, lib);
    }
}

void MapLibRemoveLib(lib_t* maplib, library_t* lib)
{
    if(!maplib || !lib)
        return;
    int idx = 0;
    while(idx<maplib->libsz && maplib->libraries[idx].lib!=lib) ++idx;
    if(idx==maplib->libsz)  //not found
        return;
    --maplib->libsz;
    if(idx!=(maplib->libsz))
        memmove(&maplib->libraries[idx], &maplib->libraries[idx+1], sizeof(onelib_t)*(maplib->libsz-idx));
    maplib->libraries[maplib->libsz].lib = NULL;
    maplib->libraries[maplib->libsz].name = NULL;
}

EXPORTDYN
int AddNeededLib(lib_t* maplib, needed_libs_t* neededlibs, int local, const char* path, box86context_t* box86, x86emu_t* emu)
{
    printf_log(LOG_DEBUG, "Trying to add \"%s\" to maplib%s\n", path, local?" (local)":"");
    // first check if lib is already loaded
    library_t *lib = getLib(my_context->maplib, path);
    if(lib) {
        add_neededlib(neededlibs, lib);
        printf_log(LOG_DEBUG, "Already present in maplib => success\n");
        return 0;
    }
    // check also in the local loaded lib
    lib = getLib(my_context->local_maplib, path);
    if(lib) {
        printf_log(LOG_DEBUG, "Already present in local_maplib => success\n");
        if(local) {
            // add lib to maplib...
            if(maplib) {
                if(lib->maplib) {
                    MapLibAddMapLib(maplib, lib->maplib);
                }
                if(!libraryInMapLib(maplib, lib))
                    MapLibAddLib(maplib, lib);
            }
        } else {
            // promote lib from local to global...
            // for add the depending local libs...
            if(lib->maplib) {
                MapLibAddMapLib(my_context->maplib, lib->maplib);
                free(lib->maplib);
                lib->maplib = NULL;
            }
            if(!libraryInMapLib(my_context->maplib, lib))
                MapLibAddLib(my_context->maplib, lib);
            MapLibRemoveLib(my_context->local_maplib, lib);
        }
        add_neededlib(neededlibs, lib);
        return 0;
    }
    // load a new one
    lib = NewLibrary(path, box86);
    if(!lib) {
        printf_log(LOG_DEBUG, "Faillure to create lib => fail\n");
        return 1;   //Error
    }

    add_neededlib(neededlibs, lib);

    // add lib now
    if(local) {
        MapLibAddLib(my_context->local_maplib, lib);
        if(maplib)
            MapLibAddLib(maplib, lib);
        lib->maplib = NewLibrarian(box86, 0);
        MapLibAddLib(lib->maplib, lib);
    } else {
        MapLibAddLib(my_context->maplib, lib);
    }

    if(!maplib)
        maplib = (local)?lib->maplib:my_context->maplib;

    int mainelf = GetElfIndex(lib);

    if(mainelf==-1) {
        // It's a native libs, just add wrapped symbols to global map
        if(AddSymbolsLibrary(maplib, lib, emu)) {   // also add needed libs
            printf_log(LOG_DEBUG, "Failure to Add lib => fail\n");
            return 1;
        }
    } else {
        // it's an emulated lib, 
        // lets load dependancies before adding symbols and launch init sequence
        if(LoadNeededLibs(box86->elfs[mainelf], maplib, &lib->needed, 0, box86, emu)) {
            printf_log(LOG_DEBUG, "Failure to Add dependant lib => fail\n");
            return 1;
        }
        // some special case, where dependancies may not be correct
        if(!strcmp(GetNameLib(lib), "libCgGL.so")) {
            AddNeededLib(maplib, &lib->needed, 0, "libGL.so.1", box86, emu);
        }
        if(!strcmp(GetNameLib(lib), "libmss.so.6")) {
            AddNeededLib(maplib, &lib->needed, 0, "libSDL-1.2.so.0", box86, emu);
            AddNeededLib(maplib, &lib->needed, 0, "libdl.so.2", box86, emu);
        }
        // add symbols
        if(AddSymbolsLibrary(maplib, lib, emu)) {   // also add needed libs
            printf_log(LOG_DEBUG, "Failure to Add lib => fail\n");
            return 1;
        }

        // finalize the lib
        if(FinalizeLibrary(lib, local?maplib:NULL, emu)) {
            printf_log(LOG_DEBUG, "Failure to finalizing lib => fail\n");
            return 1;
        }
    }
    // success
    printf_log(LOG_DEBUG, "Created lib and added to maplib => success\n");
    
    return 0;
}

library_t* GetLibMapLib(lib_t* maplib, const char* name)
{
    printf_log(LOG_DEBUG, "Trying to Get \"%s\" to maplib\n", name);
    return getLib(maplib, name);
}

library_t* GetLibInternal(const char* name)
{
    printf_log(LOG_DEBUG, "Trying to Get \"%s\" to maplib\n", name);
    library_t* lib = getLib(my_context->maplib, name);
    if(!lib) lib = getLib(my_context->local_maplib, name);
    return lib;
}

EXPORTDYN
uintptr_t FindGlobalSymbol(lib_t *maplib, const char* name)
{
    uintptr_t start = 0, end = 0;
    if(GetGlobalSymbolStartEnd(maplib, name, &start, &end))
        return start;
    return 0;
}

int GetNoSelfSymbolStartEnd(lib_t *maplib, const char* name, uintptr_t* start, uintptr_t* end, elfheader_t* self)
{
    //excude self if defined
    if(maplib->context->elfs[0]!=self) {
        if(GetSymbolStartEnd(maplib->mapsymbols, name, start, end))
            if(*start)
                return 1;
        if(GetSymbolStartEnd(maplib->weaksymbols, name, start, end))
            if(*start)
                return 1;
    }
    for(int i=0; i<maplib->libsz; ++i) {
        if(GetElfIndex(maplib->libraries[i].lib)==-1 || (maplib->context->elfs[GetElfIndex(maplib->libraries[i].lib)]!=self))
            if(GetLibSymbolStartEnd(maplib->libraries[i].lib, name, start, end))
                if(*start)
                    return 1;
    }
    // if self defined, give it another chance with self...
    if(self) {
        if(maplib->context->elfs[0]==self) {
            if(GetSymbolStartEnd(maplib->mapsymbols, name, start, end))
                if(*start)
                    return 1;
            if(GetSymbolStartEnd(maplib->weaksymbols, name, start, end))
                if(*start)
                    return 1;
        }
        for(int i=0; i<maplib->libsz; ++i) {
            if(GetElfIndex(maplib->libraries[i].lib)!=-1 && (maplib->context->elfs[GetElfIndex(maplib->libraries[i].lib)]==self))
                if(GetLibSymbolStartEnd(maplib->libraries[i].lib, name, start, end))
                    if(*start)
                        return 1;
        }
    }
    // nope, not found
    return 0;
}
static int GetGlobalSymbolStartEnd_internal(lib_t *maplib, const char* name, uintptr_t* start, uintptr_t* end)
{
    // search non-weak symbol, from older to newer (first GLOBAL object wins)
    if(GetSymbolStartEnd(maplib->mapsymbols, name, start, end))
        if(*start)
            return 1;
    // TODO: create a temporary map to search lib only 1 time, and in order of needed...
    // search in needed libs from neededlibs first, in order
    for(int i=0; i<my_context->neededlibs.size; ++i)
        if(GetLibNoWeakSymbolStartEnd(my_context->neededlibs.libs[i], name, start, end))
            if(*start)
                return 1;
    // search in global symbols
    for(int i=0; i<maplib->libsz; ++i) {
        if(GetLibNoWeakSymbolStartEnd(maplib->libraries[i].lib, name, start, end))
            if(*start)
                return 1;
    }

    // library from newer to older, weak only now
    for(int i=maplib->libsz-1; i>=0; --i) {
        if(GetLibSymbolStartEnd(maplib->libraries[i].lib, name, start, end))    // only weak symbol haven't been found yet
            if(*start)
                return 1;
    }
    if(GetSymbolStartEnd(maplib->weaksymbols, name, start, end))
        if(*start)
            return 1;
    // nope, not found
    return 0;
}
void** my_GetGTKDisplay();
void** my_GetGthreadsGotInitialized();
int GetGlobalSymbolStartEnd(lib_t *maplib, const char* name, uintptr_t* start, uintptr_t* end)
{
    if(GetGlobalSymbolStartEnd_internal(maplib, name, start, end)) {
        if(start && end && *end==*start) {  // object is of 0 sized, try to see an "_END" object of null size
            uintptr_t start2, end2;
            char* buff = (char*)malloc(strlen(name) + strlen("_END") + 1);
            strcpy(buff, name);
            strcat(buff, "_END");
            if(GetGlobalSymbolStartEnd_internal(maplib, buff, &start2, &end2)) {
                if(end2>*end && start2==end2)
                    *end = end2;
            }
            free(buff);
        }
        return 1;
    }
    // some special case symbol, defined inside box86 itself
    if(!strcmp(name, "gdk_display")) {
        *start = (uintptr_t)my_GetGTKDisplay();
        *end = *start+sizeof(void*);
        printf_log(LOG_INFO, "Using global gdk_display for gdk-x11 (%p:%p)\n", start, *(void**)start);
        return 1;
    }
    if(!strcmp(name, "g_threads_got_initialized")) {
        *start = (uintptr_t)my_GetGthreadsGotInitialized();
        *end = *start+sizeof(int);
        printf_log(LOG_INFO, "Using global g_threads_got_initialized for gthread2 (%p:%p)\n", start, *(void**)start);
        return 1;
    }
    // not found...
    return 0;
}

elfheader_t* GetGlobalSymbolElf(lib_t *maplib, const char* name)
{
    uintptr_t start = 0;
    uintptr_t end = 0;
    if(GetSymbolStartEnd(maplib->mapsymbols, name, &start, &end))
        return maplib->context->elfs[0];
    if(GetSymbolStartEnd(maplib->weaksymbols, name, &start, &end))
        return maplib->context->elfs[0];
    for(int i=0; i<maplib->libsz; ++i) {
        if(GetLibSymbolStartEnd(maplib->libraries[i].lib, name, &start, &end)) {
            int idx = GetElfIndex(maplib->libraries[i].lib);
            if(idx==-1) {
                printf_log(LOG_NONE, "Warning, getting Elf info for a native symbol \"%s\" from lib \"%s\"\n", name, GetNameLib(maplib->libraries[i].lib));
                return NULL;
            }
            return maplib->context->elfs[idx];
        }
    }
    // nope, not found
    return NULL;
}

int GetGlobalNoWeakSymbolStartEnd(lib_t *maplib, const char* name, uintptr_t* start, uintptr_t* end)
{
    if(GetSymbolStartEnd(maplib->mapsymbols, name, start, end))
        if(*start || *end)
            return 1;
    for(int i=0; i<maplib->libsz; ++i)
        if(GetLibNoWeakSymbolStartEnd(maplib->libraries[i].lib, name, start, end))
            if(*start || *end)
                return 1;
    // nope, not found
    return 0;
}

int IsGlobalNoWeakSymbolInNative(lib_t *maplib, const char* name)
{
    uintptr_t start, end;
    for(int i=0; i<maplib->libsz; ++i)
        if(GetElfIndex(maplib->libraries[i].lib)==-1)
            if(GetLibNoWeakSymbolStartEnd(maplib->libraries[i].lib, name, &start, &end))
                if(start || end)
                    return 1;
    // nope, not found
    return 0;

}

int GetLocalSymbolStartEnd(lib_t *maplib, const char* name, uintptr_t* start, uintptr_t* end, elfheader_t *self)
{
    if(maplib->context->elfs[0]==self || !self) {
        if(GetSymbolStartEnd(maplib->localsymbols, name, start, end))
            if(*start || *end)
                return 1;
        if(self)
            return 0;
    }
    for(int i=0; i<maplib->libsz; ++i) {
        if(GetElfIndex(maplib->libraries[i].lib)!=-1 && (!self || maplib->context->elfs[GetElfIndex(maplib->libraries[i].lib)]==self)) {
            if(GetLibLocalSymbolStartEnd(maplib->libraries[i].lib, name, start, end))
                if(*start)
                    return 1;
            if(self)
                return 0;
        }
    }
    return 0;
}

int GetSelfSymbolStartEnd(lib_t *maplib, const char* name, uintptr_t* start, uintptr_t* end, elfheader_t *self)
{
    if(maplib->context->elfs[0]==self) {
        if(GetSymbolStartEnd(maplib->localsymbols, name, start, end))
            if(*start || *end)
                return 1;
    } else {
        for(int i=0; i<maplib->libsz; ++i) {
            if(GetElfIndex(maplib->libraries[i].lib)!=-1 && (maplib->context->elfs[GetElfIndex(maplib->libraries[i].lib)]==self))
                if(GetLibSymbolStartEnd(maplib->libraries[i].lib, name, start, end))
                    if(*start || *end)
                        return 1;
        }
    }
    return 0;
}

int GetNoWeakSymbolStartEnd(lib_t *maplib, const char* name, uintptr_t* start, uintptr_t* end, elfheader_t *self)
{
    if(maplib->context->elfs[0]==self) {
        if(GetSymbolStartEnd(maplib->mapsymbols, name, start, end))
            if(*start || *end)
                return 1;
    } else {
        for(int i=0; i<maplib->libsz; ++i) {
            if(GetElfIndex(maplib->libraries[i].lib)!=-1 && (maplib->context->elfs[GetElfIndex(maplib->libraries[i].lib)]==self))
                if(GetLibNoWeakSymbolStartEnd(maplib->libraries[i].lib, name, start, end))
                    if(*start || *end)
                        return 1;
        }
    }
    return 0;
}


void AddSymbol(kh_mapsymbols_t *mapsymbols, const char* name, uintptr_t addr, uint32_t sz)
{
    int ret;
    khint_t k = kh_put(mapsymbols, mapsymbols, name, &ret);
    kh_value(mapsymbols, k).offs = addr;
    kh_value(mapsymbols, k).sz = sz;
}
uintptr_t FindSymbol(kh_mapsymbols_t *mapsymbols, const char* name)
{
    khint_t k = kh_get(mapsymbols, mapsymbols, name);
    if(k==kh_end(mapsymbols))
        return 0;
    return kh_val(mapsymbols, k).offs;
}
void AddWeakSymbol(kh_mapsymbols_t *mapsymbols, const char* name, uintptr_t addr, uint32_t sz)
{
    int ret;
    khint_t k = kh_put(mapsymbols, mapsymbols, name, &ret);
    if(ret==1)
        return; // Symbol already there, don't touch it
    kh_value(mapsymbols, k).offs = addr;
    kh_value(mapsymbols, k).sz = sz;
}

int GetSymbolStartEnd(kh_mapsymbols_t* mapsymbols, const char* name, uintptr_t* start, uintptr_t* end)
{
    khint_t k = kh_get(mapsymbols, mapsymbols, name);
    if(k==kh_end(mapsymbols))
        return 0;
    *start = kh_val(mapsymbols, k).offs;
    *end = *start + kh_val(mapsymbols, k).sz;
    return 1;
}

const char* GetSymbolName(kh_mapsymbols_t* mapsymbols, void* p, uintptr_t* start, uint32_t* sz)
{
    uintptr_t addr = (uintptr_t)p;
    onesymbol_t *one;
    kh_foreach_value_ref(mapsymbols, one, 
        if((one->offs >= addr) && (one->offs+one->sz<addr)) {
            *start  = one->offs;
            *sz = one->sz;
            return kh_key(mapsymbols, __i);
        }
    );
    return NULL;
}

const char* FindSymbolName(lib_t *maplib, void* p, void** start, uint32_t* sz, const char** libname, void** base)
{
    // first, search in self...
    const char* ret = NULL;
    uintptr_t offs = 0;
    uint32_t size = 0;
    elfheader_t* h = FindElfAddress(my_context, (uintptr_t)p);
    if(h) {
        ret = FindNearestSymbolName(h, p, &offs, &size);
    }
    if(h) {
        if(start)
            *start = (void*)offs;
        if(sz)
            *sz = size;
        if(libname)
            *libname = ElfName(h);
        if(base)
            *base = GetBaseAddress(h);
        return ret;
    }
    // TODO: then search in the other libs...
    return NULL;
}

void AddOffsetSymbol(lib_t *maplib, void* offs, const char* name)
{
    int ret;
    khint_t k = kh_put(mapoffsets, maplib->mapoffsets, (uintptr_t)offs, &ret);
    kh_value(maplib->mapoffsets, k) = (cstr_t)name;
}

const char* GetNameOffset(lib_t *maplib, void* offs)
{
    if(!maplib || !maplib->mapoffsets)
        return NULL;
    khint_t k = kh_get(mapoffsets, maplib->mapoffsets, (uintptr_t)offs);
    if (k!=kh_end(maplib->mapoffsets))
        return kh_value(maplib->mapoffsets, k);
    return NULL;
}
