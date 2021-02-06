#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <elf.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <link.h>
#include <unistd.h>
#include <errno.h>

#include "box86version.h"
#include "elfloader.h"
#include "debug.h"
#include "elfload_dump.h"
#include "elfloader_private.h"
#include "librarian.h"
#include "x86run.h"
#include "bridge.h"
#include "wrapper.h"
#include "box86context.h"
#include "library.h"
#include "x86emu.h"
#include "box86stack.h"
#include "callback.h"
#include "dynarec.h"
#include "box86stack.h"
#include "custommem.h"
#ifdef DYNAREC
#include "dynablock.h"
#endif
#include "../emu/x86emu_private.h"
#include "x86tls.h"

void* my__IO_2_1_stderr_ = NULL;
void* my__IO_2_1_stdin_  = NULL;
void* my__IO_2_1_stdout_ = NULL;

// return the index of header (-1 if it doesn't exist)
int getElfIndex(box86context_t* ctx, elfheader_t* head) {
    for (int i=0; i<ctx->elfsize; ++i)
        if(ctx->elfs[i]==head)
            return i;
    return -1;
}

elfheader_t* LoadAndCheckElfHeader(FILE* f, const char* name, int exec)
{
    elfheader_t *h = ParseElfHeader(f, name, exec);
    if(!h)
        return NULL;

    if ((h->path = realpath(name, NULL)) == NULL) {
        h->path = (char*)malloc(1);
        h->path[0] = '\0';
    }
    return h;
}

void FreeElfHeader(elfheader_t** head)
{
    if(!head || !*head)
        return;
    elfheader_t *h = *head;
#ifdef DYNAREC
    /*if(h->text) {
        dynarec_log(LOG_INFO, "Free Dynarec block for %s\n", h->path);
        cleanDBFromAddressRange(my_context, h->text, h->textsz, 1);
    }*/ // will be free at the end, no need to free it now
#endif
    free(h->name);
    free(h->path);
    free(h->PHEntries);
    free(h->SHEntries);
    free(h->SHStrTab);
    free(h->StrTab);
    free(h->Dynamic);
    free(h->DynStr);
    free(h->SymTab);
    free(h->DynSym);

    FreeElfMemory(h);
    free(h);

    *head = NULL;
}

int CalcLoadAddr(elfheader_t* head)
{
    head->memsz = 0;
    head->paddr = head->vaddr = ~(uintptr_t)0;
    head->align = 1;
    for (int i=0; i<head->numPHEntries; ++i)
        if(head->PHEntries[i].p_type == PT_LOAD) {
            if(head->paddr > (uintptr_t)head->PHEntries[i].p_paddr)
                head->paddr = (uintptr_t)head->PHEntries[i].p_paddr;
            if(head->vaddr > (uintptr_t)head->PHEntries[i].p_vaddr)
                head->vaddr = (uintptr_t)head->PHEntries[i].p_vaddr;
        }
    
    if(head->vaddr==~(uintptr_t)0 || head->paddr==~(uintptr_t)0) {
        printf_log(LOG_NONE, "Error: v/p Addr for Elf Load not set\n");
        return 1;
    }

    head->stacksz = 1024*1024;          //1M stack size default?
    head->stackalign = 4;   // default align for stack
    for (int i=0; i<head->numPHEntries; ++i) {
        if(head->PHEntries[i].p_type == PT_LOAD) {
            uintptr_t phend = head->PHEntries[i].p_vaddr - head->vaddr + head->PHEntries[i].p_memsz;
            if(phend > head->memsz)
                head->memsz = phend;
            if(head->PHEntries[i].p_align > head->align)
                head->align = head->PHEntries[i].p_align;
        }
        if(head->PHEntries[i].p_type == PT_GNU_STACK) {
            if(head->stacksz < head->PHEntries[i].p_memsz)
                head->stacksz = head->PHEntries[i].p_memsz;
            if(head->stackalign < head->PHEntries[i].p_align)
                head->stackalign = head->PHEntries[i].p_align;
        }
        if(head->PHEntries[i].p_type == PT_TLS) {
            head->tlssize = head->PHEntries[i].p_memsz;
            head->tlsalign = head->PHEntries[i].p_align;
            // force alignement...
            if(head->tlsalign>1)
                while(head->tlssize&(head->tlsalign-1))
                    head->tlssize++;
        }
    }
    printf_log(LOG_DEBUG, "Elf Addr(v/p)=%p/%p Memsize=0x%x (align=0x%x)\n", (void*)head->vaddr, (void*)head->paddr, head->memsz, head->align);
    printf_log(LOG_DEBUG, "Elf Stack Memsize=%u (align=%u)\n", head->stacksz, head->stackalign);
    printf_log(LOG_DEBUG, "Elf TLS Memsize=%u (align=%u)\n", head->tlssize, head->tlsalign);

    return 0;
}

const char* ElfName(elfheader_t* head)
{
    if(!head)
        return "(noelf)";
    return head->name;
}

int AllocElfMemory(box86context_t* context, elfheader_t* head, int mainbin)
{
    uintptr_t offs = 0;
    if(mainbin && head->vaddr==0) {
        char* load_addr = getenv("BOX86_LOAD_ADDR");
        if(load_addr)
            if(sscanf(load_addr, "0x%x", &offs)!=1)
                offs = 0;
    }
    if(!offs)
        offs = head->vaddr;
    if(head->vaddr) {
        head->multiblock_n = 0; // count PHEntrie with LOAD
        for (int i=0; i<head->numPHEntries; ++i) 
            if(head->PHEntries[i].p_type == PT_LOAD && head->PHEntries[i].p_flags)
                ++head->multiblock_n;
        head->multiblock_size = (uint32_t*)calloc(head->multiblock_n, sizeof(uint32_t));
        head->multiblock_offs = (uintptr_t*)calloc(head->multiblock_n, sizeof(uintptr_t));
        head->multiblock = (void**)calloc(head->multiblock_n, sizeof(void*));
        // and now, create all individual blocks
        head->memory = (char*)0xffffffff;
        int n = 0;
        for (int i=0; i<head->numPHEntries; ++i) 
            if(head->PHEntries[i].p_type == PT_LOAD && head->PHEntries[i].p_flags) {
                Elf32_Phdr * e = &head->PHEntries[i];
                uintptr_t bstart = e->p_vaddr;
                uint32_t bsize = e->p_memsz;
                uintptr_t balign = e->p_align;
                if (balign) balign = balign-1; else balign = 1;
                if(balign<4095) balign = 4095;
                uintptr_t bend = (bstart + bsize + balign)&(~balign);
                bstart &= ~balign;
                int ok = 0;
                for (int j=0; !ok && j<n; ++j) {
                    uintptr_t start = head->multiblock_offs[j];
                    uintptr_t end = head->multiblock_offs[j] + head->multiblock_size[j];
                    start &= ~balign;
                    if(((bstart>=start) && (bstart<=end)) || ((bend>=start) && (bend<=end)) || ((bstart<start) && (bend>end))) {
                        // merge
                        ok = 1;
                        if(bstart<start)
                            head->multiblock_offs[j] = bstart;
                        head->multiblock_size[j] = ((bend>end)?bend:end) - head->multiblock_offs[j];
                        --head->multiblock_n;
                    }
                }
                if(!ok) {
                    head->multiblock_offs[n] = bstart;
                    head->multiblock_size[n] = bend - head->multiblock_offs[n];
                    ++n;
                }
            }
        head->multiblock_n = n; // might be less in fact
        for (int i=0; i<head->multiblock_n; ++i) {
            
            printf_log(LOG_DEBUG, "Allocating 0x%x memory @%p for Elf \"%s\"\n", head->multiblock_size[i], (void*)head->multiblock_offs[i], head->name);
            void* p = mmap((void*)head->multiblock_offs[i], head->multiblock_size[i]
                , PROT_READ | PROT_WRITE | PROT_EXEC
                , MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED
                , -1, 0);
            if(p==MAP_FAILED) {
                printf_log(LOG_NONE, "Cannot create memory map (@%p 0x%x/0x%x) for elf \"%s\"\n", (void*)head->multiblock_offs[i], head->multiblock_size[i], head->align, head->name);
                return 1;
            }
            setProtection((uintptr_t)p, head->multiblock_size[i], PROT_READ | PROT_WRITE | PROT_EXEC);
            head->multiblock[i] = p;
            if(p<(void*)head->memory)
                head->memory = (char*)p;
        }
    } else {
        // vaddr is 0, load everything has a One block
        printf_log(LOG_DEBUG, "Allocating 0x%x memory @%p for Elf \"%s\"\n", head->memsz, (void*)offs, head->name);
        void* p = mmap((void*)offs, head->memsz
            , PROT_READ | PROT_WRITE | PROT_EXEC
            , MAP_PRIVATE | MAP_ANONYMOUS | (((offs)?MAP_FIXED:0))
            , -1, 0);
        if(p==MAP_FAILED) {
            printf_log(LOG_NONE, "Cannot create memory map (@%p 0x%x/0x%x) for elf \"%s\"\n", (void*)offs, head->memsz, head->align, head->name);
            return 1;
        }
        setProtection((uintptr_t)p, head->memsz, PROT_READ | PROT_WRITE | PROT_EXEC);
        head->memory = p;
        memset(p, 0, head->memsz);
        head->delta = (intptr_t)p - (intptr_t)head->vaddr;
        printf_log(LOG_DEBUG, "Got %p (delta=%p)\n", p, (void*)head->delta);

        head->multiblock_n = 1;
        head->multiblock_size = (uint32_t*)calloc(head->multiblock_n, sizeof(uint32_t));
        head->multiblock_offs = (uintptr_t*)calloc(head->multiblock_n, sizeof(uintptr_t));
        head->multiblock = (void**)calloc(head->multiblock_n, sizeof(void*));
        head->multiblock_size[0] = head->memsz;
        head->multiblock_offs[0] = (uintptr_t)p;
        head->multiblock[0] = p;
    }

    head->tlsbase = AddTLSPartition(context, head->tlssize);

    return 0;
}

void FreeElfMemory(elfheader_t* head)
{
    if(head->multiblock_n) {
        for(int i=0; i<head->multiblock_n; ++i)
            munmap(head->multiblock[i], head->multiblock_size[i]);
        free(head->multiblock);
        free(head->multiblock_size);
        free(head->multiblock_offs);
    }
}

int LoadElfMemory(FILE* f, box86context_t* context, elfheader_t* head)
{
    for (int i=0; i<head->numPHEntries; ++i) {
        if(head->PHEntries[i].p_type == PT_LOAD) {
            Elf32_Phdr * e = &head->PHEntries[i];
            char* dest = (char*)e->p_paddr + head->delta;
            printf_log(LOG_DEBUG, "Loading block #%i @%p (0x%x/0x%x)\n", i, dest, e->p_filesz, e->p_memsz);
            fseeko64(f, e->p_offset, SEEK_SET);
            if(e->p_filesz) {
                if(fread(dest, e->p_filesz, 1, f)!=1) {
                    printf_log(LOG_NONE, "Fail to read PT_LOAD part #%d (size=%d)\n", i, e->p_filesz);
                    return 1;
                }
            }
#ifdef DYNAREC
            if(e->p_flags & PF_X) {
                dynarec_log(LOG_DEBUG, "Add ELF eXecutable Memory %p:%p\n", dest, (void*)e->p_memsz);
                addDBFromAddressRange((uintptr_t)dest, e->p_memsz);
            }
#endif

            // zero'd difference between filesz and memsz
            /*if(e->p_filesz != e->p_memsz)
                memset(dest+e->p_filesz, 0, e->p_memsz - e->p_filesz);*/    //block is already 0'd at creation
        }
        if(head->PHEntries[i].p_type == PT_TLS) {
            Elf32_Phdr * e = &head->PHEntries[i];
            char* dest = (char*)(context->tlsdata+context->tlssize+head->tlsbase);
            printf_log(LOG_DEBUG, "Loading TLS block #%i @%p (0x%x/0x%x)\n", i, dest, e->p_filesz, e->p_memsz);
            if(e->p_filesz) {
                fseeko64(f, e->p_offset, SEEK_SET);
                if(fread(dest, e->p_filesz, 1, f)!=1) {
                    printf_log(LOG_NONE, "Fail to read PT_TLS part #%d (size=%d)\n", i, e->p_filesz);
                    return 1;
                }
            }
            // zero'd difference between filesz and memsz
            if(e->p_filesz != e->p_memsz)
                memset(dest+e->p_filesz, 0, e->p_memsz - e->p_filesz);
        }
    }
    return 0;
}

int ReloadElfMemory(FILE* f, box86context_t* context, elfheader_t* head)
{
    for (int i=0; i<head->numPHEntries; ++i) {
        if(head->PHEntries[i].p_type == PT_LOAD) {
            Elf32_Phdr * e = &head->PHEntries[i];
            char* dest = (char*)e->p_paddr + head->delta;
            printf_log(LOG_DEBUG, "Re-loading block #%i @%p (0x%x/0x%x)\n", i, dest, e->p_filesz, e->p_memsz);
            int ret = fseeko64(f, e->p_offset, SEEK_SET);
            if(ret==-1) {printf_log(LOG_NONE, "Fail to (re)seek PT_LOAD part #%d (offset=%d, errno=%d/%s)\n", i, e->p_offset, errno, strerror(errno)); return 1;}
            if(e->p_filesz) {
                ssize_t r = -1;
                #ifdef DYNAREC
                unprotectDB((uintptr_t)dest, e->p_memsz);
                #endif
                if((r=fread(dest, e->p_filesz, 1, f))!=1) {
                    printf_log(LOG_NONE, "Fail to (re)read PT_LOAD part #%d (dest=%p, size=%d, return=%d, feof=%d/ferror=%d/%s)\n", i, dest, e->p_filesz, r, feof(f), ferror(f), strerror(ferror(f)));
                    return 1;
                }
            }
            // zero'd difference between filesz and memsz
            if(e->p_filesz != e->p_memsz)
                memset(dest+e->p_filesz, 0, e->p_memsz - e->p_filesz);
        }
    }
    // TLS data are just a copy, no need to re-load it
    return 0;
}
int FindR386COPYRel(elfheader_t* h, const char* name, uintptr_t *offs, uint32_t** p)
{
    if(!h)
        return 0;
    Elf32_Rel * rel = (Elf32_Rel *)(h->rel + h->delta);
    if(!h->rel)
        return 0;
    int cnt = h->relsz / h->relent;
    for (int i=0; i<cnt; ++i) {
        int t = ELF32_R_TYPE(rel[i].r_info);
        Elf32_Sym *sym = &h->DynSym[ELF32_R_SYM(rel[i].r_info)];
        const char* symname = SymName(h, sym);
        if(!strcmp(symname, name) && t==R_386_COPY) {
            *offs = sym->st_value + h->delta;
            *p = (uint32_t*)(rel[i].r_offset + h->delta);
            return 1;
        }
    }
    return 0;
}

int RelocateElfREL(lib_t *maplib, lib_t *local_maplib, elfheader_t* head, int cnt, Elf32_Rel *rel)
{
    for (int i=0; i<cnt; ++i) {
        int t = ELF32_R_TYPE(rel[i].r_info);
        Elf32_Sym *sym = &head->DynSym[ELF32_R_SYM(rel[i].r_info)];
        int bind = ELF32_ST_BIND(sym->st_info);
        const char* symname = SymName(head, sym);
        uint32_t ndx = sym->st_shndx;
        uint32_t *p = (uint32_t*)(rel[i].r_offset + head->delta);
        uintptr_t offs = 0;
        uintptr_t end = 0;
        uintptr_t tmp = 0;
        elfheader_t* h_tls = head;
        if(bind==STB_LOCAL) {
            offs = sym->st_value + head->delta;
            end = offs + sym->st_size;
        } else {
            // this is probably very very wrong. A proprer way to get reloc need to be writen, but this hack seems ok for now
            // at least it work for half-life, unreal, ut99, zsnes, Undertale, ColinMcRae Remake, FTL, ShovelKnight...
            if(bind==STB_GLOBAL && (ndx==10 || ndx==19) && t!=R_386_GLOB_DAT) {
                offs = sym->st_value + head->delta;
                end = offs + sym->st_size;
            }
            // so weak symbol are the one left
            if(!offs && !end) {
                h_tls = NULL;
                if(local_maplib)
                    GetGlobalSymbolStartEnd(local_maplib, symname, &offs, &end);
                if(!offs && !end)
                    GetGlobalSymbolStartEnd(maplib, symname, &offs, &end);
            }
        }
        uintptr_t globoffs, globend;
        uint32_t* globp;
        int delta;
        switch(t) {
            case R_386_NONE:
                // can be ignored
                printf_log(LOG_DUMP, "Ignoring %s @%p (%p)\n", DumpRelType(t), p, (void*)(p?(*p):0));
                break;
            case R_386_TLS_TPOFF:
                // Negated offset in static TLS block
                {
                    if(h_tls)
                        offs = sym->st_value;
                    else {
                        if(local_maplib)
                            h_tls = GetGlobalSymbolElf(local_maplib, symname);
                        if(!h_tls)
                            h_tls = GetGlobalSymbolElf(maplib, symname);
                    }
                    if(h_tls) {
                        delta = *(int*)p;
                        printf_log(LOG_DUMP, "Applying %s %s on %s @%p (%d -> %d)\n", (bind==STB_LOCAL)?"Local":"Global", DumpRelType(t), symname, p, delta, (int32_t)offs + h_tls->tlsbase);
                        *p = (uint32_t)((int32_t)offs + h_tls->tlsbase);
                    } else {
                        printf_log(LOG_INFO, "Warning, cannot apply %s %s on %s @%p (%d), no elf_header found\n", (bind==STB_LOCAL)?"Local":"Global", DumpRelType(t), symname, p, (int32_t)offs);
                    }
                }
                break;
            case R_386_PC32:
                    if (!offs) {
                        printf_log(LOG_NONE, "Error: Global Symbol %s not found, cannot apply R_386_PC32 @%p (%p) in %s\n", symname, p, *(void**)p, head->name);
                    }
                    offs = (offs - (uintptr_t)p);
                    if(!offs)
                        printf_log(LOG_DUMP, "Apply %s R_386_PC32 @%p with sym=%s (%p -> %p)\n", (bind==STB_LOCAL)?"Local":"Global", p, symname, *(void**)p, (void*)(*(uintptr_t*)p+offs));
                    *p += offs;
                break;
            case R_386_GLOB_DAT:
                if(!IsGlobalNoWeakSymbolInNative(maplib, symname) && FindR386COPYRel(my_context->elfs[0], symname, &globoffs, &globp)) {
                    // set global offs / size for the symbol
                    offs = sym->st_value + head->delta;
                    end = offs + sym->st_size;
                    printf_log(LOG_DUMP, "Apply %s R_386_GLOB_DAT with R_386_COPY @%p/%p (%p/%p -> %p/%p) on sym=%s\n", (bind==STB_LOCAL)?"Local":"Global", p, globp, (void*)(p?(*p):0), (void*)(globp?(*globp):0), (void*)offs, (void*)globoffs, symname);
                    memmove(globp, (void*)offs, sym->st_size);
                    if(LOG_DUMP<=box86_log) {
                        uint32_t*k = (uint32_t*)globp;
                        for (int i=0; i<((sym->st_size>128)?128:sym->st_size); i+=4, ++k)
                            printf_log(LOG_DUMP, "%s0x%08X", i?" ":"", *k);
                        printf_log(LOG_DUMP, "%s)\n", (sym->st_size>128)?" ...":"");
                    }
                    *p = globoffs;
                } else {
                    // Look for same symbol already loaded but not in self (so no need for local_maplib here)
                    if (GetGlobalNoWeakSymbolStartEnd(maplib, symname, &globoffs, &globend)) {
                        offs = globoffs;
                        end = globend;
                    }
                    if (!offs) {
                        if(strcmp(symname, "__gmon_start__"))
                            printf_log(LOG_NONE, "Error: Global Symbol %s not found, cannot apply R_386_GLOB_DAT @%p (%p) in %s\n", symname, p, *(void**)p, head->name);
    //                    return -1;
                    } else {
                        printf_log(LOG_DUMP, "Apply %s R_386_GLOB_DAT @%p (%p -> %p) on sym=%s\n", (bind==STB_LOCAL)?"Local":"Global", p, (void*)(p?(*p):0), (void*)offs, symname);
                        *p = offs;
                    }
                }
                break;
            case R_386_RELATIVE:
                printf_log(LOG_DUMP, "Apply %s R_386_RELATIVE @%p (%p -> %p)\n", (bind==STB_LOCAL)?"Local":"Global", p, *(void**)p, (void*)((*p)+head->delta));
                *p += head->delta;
                break;
            case R_386_32:
                if (!offs) {
                    printf_log(LOG_NONE, "Error: Symbol %s not found, cannot apply R_386_32 @%p (%p) in %s\n", symname, p, *(void**)p, head->name);
//                    return -1;
                } else {
                    printf_log(LOG_DUMP, "Apply %s R_386_32 @%p with sym=%s (%p -> %p)\n", (bind==STB_LOCAL)?"Local":"Global", p, symname, *(void**)p, (void*)(offs+*(uint32_t*)p));
                    *p += offs;
                }
                break;
            case R_386_TLS_DTPMOD32:
                // ID of module containing symbol
                if(!symname || symname[0]=='\0' || bind==STB_LOCAL)
                    offs = getElfIndex(my_context, head);
                else {
                    if(!h_tls) {
                        if(local_maplib)
                            h_tls = GetGlobalSymbolElf(local_maplib, symname);
                        if(!h_tls)
                            h_tls = GetGlobalSymbolElf(maplib, symname);
                    }
                    offs = getElfIndex(my_context, h_tls);
                }
                if(p) {
                    printf_log(LOG_DUMP, "Apply %s %s @%p with sym=%s (%p -> %p)\n", "R_386_TLS_DTPMOD32", (bind==STB_LOCAL)?"Local":"Global", p, symname, *(void**)p, (void*)offs);
                    *p = offs;
                } else {
                    printf_log(LOG_NONE, "Warning, Symbol %s or Elf not found, but R_386_TLS_DTPMOD32 Slot Offset is NULL \n", symname);
                }
                break;
            case R_386_TLS_DTPOFF32:
                // Offset in TLS block
                if (!offs && !end) {
                    if(bind==STB_WEAK) {
                        printf_log(LOG_INFO, "Warning: Weak Symbol %s not found, cannot apply R_386_TLS_DTPOFF32 @%p (%p)\n", symname, p, *(void**)p);
                    } else {
                        printf_log(LOG_NONE, "Error: Symbol %s not found, cannot apply R_386_TLS_DTPOFF32 @%p (%p) in %s\n", symname, p, *(void**)p, head->name);
                    }
//                    return -1;
                } else {
                    if(h_tls)
                        offs = sym->st_value;
                    if(p) {
                        int tlsoffset = offs;    // it's not an offset in elf memory
                        printf_log(LOG_DUMP, "Apply %s R_386_TLS_DTPOFF32 @%p with sym=%s (%p -> %p)\n", (bind==STB_LOCAL)?"Local":"Global", p, symname, (void*)tlsoffset, (void*)offs);
                        *p = tlsoffset;
                    } else {
                        printf_log(LOG_NONE, "Warning, Symbol %s found, but R_386_TLS_DTPOFF32 Slot Offset is NULL \n", symname);
                    }
                }
                break;
            case R_386_JMP_SLOT:
                // apply immediatly for gobject closure marshal or for LOCAL binding. Also, apply immediatly if it doesn't jump in the got
                tmp = (uintptr_t)(*p);
                if (bind==STB_LOCAL 
                  || ((symname && strstr(symname, "g_cclosure_marshal_")==symname)) 
                  || !tmp
                  || !((tmp>=head->plt && tmp<head->plt_end) || (tmp>=head->gotplt && tmp<head->gotplt_end))
                  ) {
                    if (!offs) {
                        if(bind==STB_WEAK) {
                            printf_log(LOG_INFO, "Warning: Weak Symbol %s not found, cannot apply R_386_JMP_SLOT @%p (%p)\n", symname, p, *(void**)p);
                        } else {
                            printf_log(LOG_NONE, "Error: Symbol %s not found, cannot apply R_386_JMP_SLOT @%p (%p) in %s\n", symname, p, *(void**)p, head->name);
                        }
    //                    return -1;
                    } else {
                        if(p) {
                            printf_log(LOG_DUMP, "Apply %s R_386_JMP_SLOT @%p with sym=%s (%p -> %p)\n", (bind==STB_LOCAL)?"Local":"Global", p, symname, *(void**)p, (void*)offs);
                            *p = offs;
                        } else {
                            printf_log(LOG_NONE, "Warning, Symbol %s found, but Jump Slot Offset is NULL \n", symname);
                        }
                    }
                } else {
                    printf_log(LOG_DUMP, "Preparing (if needed) %s R_386_JMP_SLOT @%p (0x%x->0x%0x) with sym=%s to be apply later\n", (bind==STB_LOCAL)?"Local":"Global", p, *p, *p+head->delta, symname);
                    *p += head->delta;
                }
                break;
            case R_386_COPY:
                if(offs) {
                    if(*p==0) {
                        offs = 0;
                        if(local_maplib)
                            GetNoSelfSymbolStartEnd(local_maplib, symname, &offs, &end, head);
                        if(!offs)
                            GetNoSelfSymbolStartEnd(maplib, symname, &offs, &end, head);   // get original copy if any
                        printf_log(LOG_DUMP, "Apply %s R_386_COPY @%p with sym=%s, @%p size=%d (", (bind==STB_LOCAL)?"Local":"Global", p, symname, (void*)offs, sym->st_size);
                        memmove(p, (void*)offs, sym->st_size);
                        if(LOG_DUMP<=box86_log) {
                            uint32_t*k = (uint32_t*)p;
                            for (int i=0; i<((sym->st_size>128)?128:sym->st_size); i+=4, ++k)
                                printf_log(LOG_DUMP, "%s0x%08X", i?" ":"", *k);
                            printf_log(LOG_DUMP, "%s)\n", (sym->st_size>128)?" ...":"");
                        }
                    } else {
                        printf_log(LOG_DUMP, "Already applied %s R_386_COPY @%p with sym=%s, @%p size=%d\n", (bind==STB_LOCAL)?"Local":"Global", p, symname, (void*)offs, sym->st_size);
                    }
                } else {
                    printf_log(LOG_NONE, "Error: Symbol %s not found, cannot apply R_386_COPY @%p (%p) in %s\n", symname, p, *(void**)p, head->name);
                }
                break;
            default:
                printf_log(LOG_INFO, "Warning, don't know of to handle rel #%d %s (%p)\n", i, DumpRelType(ELF32_R_TYPE(rel[i].r_info)), p);
        }
    }
    return 0;
}

int RelocateElfRELA(lib_t *maplib, lib_t *local_maplib, elfheader_t* head, int cnt, Elf32_Rela *rela)
{
    for (int i=0; i<cnt; ++i) {
        Elf32_Sym *sym = &head->DynSym[ELF32_R_SYM(rela[i].r_info)];
        const char* symname = SymName(head, sym);
        uint32_t *p = (uint32_t*)(rela[i].r_offset + head->delta);
        uintptr_t offs = 0;
        uintptr_t end = 0;
        switch(ELF32_R_TYPE(rela[i].r_info)) {
            case R_386_NONE:
            case R_386_PC32:
                // can be ignored
                break;
            case R_386_COPY:
                if(local_maplib)
                    GetNoSelfSymbolStartEnd(local_maplib, symname, &offs, &end, head);
                if(!offs)
                    GetNoSelfSymbolStartEnd(maplib, symname, &offs, &end, head);
                if(offs) {
                    // add r_addend to p?
                    printf_log(LOG_DUMP, "Apply R_386_COPY @%p with sym=%s, @%p size=%d\n", p, symname, (void*)offs, sym->st_size);
                    memcpy(p, (void*)(offs+rela[i].r_addend), sym->st_size);
                } else {
                    printf_log(LOG_NONE, "Error: Symbol %s not found, cannot apply RELA R_386_COPY @%p (%p) in %s\n", symname, p, *(void**)p, head->name);
                }
                break;
            default:
                printf_log(LOG_INFO, "Warning, don't know of to handle rela #%d %s on %s\n", i, DumpRelType(ELF32_R_TYPE(rela[i].r_info)), symname);
        }
    }
    return 0;
}
int RelocateElf(lib_t *maplib, lib_t *local_maplib, elfheader_t* head)
{
    if(head->rel) {
        int cnt = head->relsz / head->relent;
        DumpRelTable(head, cnt, (Elf32_Rel *)(head->rel + head->delta), "Rel");
        printf_log(LOG_DEBUG, "Applying %d Relocation(s) for %s\n", cnt, head->name);
        if(RelocateElfREL(maplib, local_maplib, head, cnt, (Elf32_Rel *)(head->rel + head->delta)))
            return -1;
    }
    if(head->rela) {
        int cnt = head->relasz / head->relaent;
        DumpRelATable(head, cnt, (Elf32_Rela *)(head->rela + head->delta), "RelA");
        printf_log(LOG_DEBUG, "Applying %d Relocation(s) with Addend for %s\n", cnt, head->name);
        if(RelocateElfRELA(maplib, local_maplib, head, cnt, (Elf32_Rela *)(head->rela + head->delta)))
            return -1;
    }
   
    return 0;
}

int RelocateElfPlt(lib_t *maplib, lib_t *local_maplib, elfheader_t* head)
{
    if(head->pltrel) {
        int cnt = head->pltsz / head->pltent;
        if(head->pltrel==DT_REL) {
            DumpRelTable(head, cnt, (Elf32_Rel *)(head->jmprel + head->delta), "PLT");
            printf_log(LOG_DEBUG, "Applying %d PLT Relocation(s) for %s\n", cnt, head->name);
            if(RelocateElfREL(maplib, local_maplib, head, cnt, (Elf32_Rel *)(head->jmprel + head->delta)))
                return -1;
        } else if(head->pltrel==DT_RELA) {
            DumpRelATable(head, cnt, (Elf32_Rela *)(head->jmprel + head->delta), "PLT");
            printf_log(LOG_DEBUG, "Applying %d PLT Relocation(s) with Addend for %s\n", cnt, head->name);
            if(RelocateElfRELA(maplib, local_maplib, head, cnt, (Elf32_Rela *)(head->jmprel + head->delta)))
                return -1;
        }
        if(pltResolver==~0) {
            pltResolver = AddBridge(my_context->system, vFE, PltResolver, 0);
        }
        if(head->pltgot) {
            *(uintptr_t*)(head->pltgot+head->delta+8) = pltResolver;
            *(uintptr_t*)(head->pltgot+head->delta+4) = (uintptr_t)head;
            printf_log(LOG_DEBUG, "PLT Resolver injected in plt.got at %p\n", (void*)(head->pltgot+head->delta+8));
        } else if(head->got) {
            *(uintptr_t*)(head->got+head->delta+8) = pltResolver;
            *(uintptr_t*)(head->got+head->delta+4) = (uintptr_t)head;
            printf_log(LOG_DEBUG, "PLT Resolver injected in got at %p\n", (void*)(head->got+head->delta+8));
        }
    }
   
    return 0;
}

void CalcStack(elfheader_t* elf, uint32_t* stacksz, int* stackalign)
{
    if(*stacksz < elf->stacksz)
        *stacksz = elf->stacksz;
    if(*stackalign < elf->stackalign)
        *stackalign = elf->stackalign;
}

Elf32_Sym* GetFunction(elfheader_t* h, const char* name)
{
    // TODO: create a hash on named to avoid this loop
    for (int i=0; i<h->numSymTab; ++i) {
        int type = ELF32_ST_TYPE(h->SymTab[i].st_info);
        if(/*h->SymTab[i].st_info == 18*/type==STT_FUNC) {    // TODO: this "18" is probably defined somewhere
            const char * symname = h->StrTab+h->SymTab[i].st_name;
            if(strcmp(symname, name)==0) {
                return h->SymTab+i;
            }
        }
    }
    return NULL;
}

Elf32_Sym* GetElfObject(elfheader_t* h, const char* name)
{
    for (int i=0; i<h->numSymTab; ++i) {
        int type = ELF32_ST_TYPE(h->SymTab[i].st_info);
        if(/*h->SymTab[i].st_info == 16*/type==STT_OBJECT) {
            const char * symname = h->StrTab+h->SymTab[i].st_name;
            if(strcmp(symname, name)==0) {
                return h->SymTab+i;
            }
        }
    }
    return NULL;
}


uintptr_t GetFunctionAddress(elfheader_t* h, const char* name)
{
    Elf32_Sym* sym = GetFunction(h, name);
    if(sym) return sym->st_value;
    return 0;
}

uintptr_t GetEntryPoint(lib_t* maplib, elfheader_t* h)
{
    uintptr_t ep = h->entrypoint + h->delta;
    printf_log(LOG_DEBUG, "Entry Point is %p\n", (void*)ep);
    if(box86_log>=LOG_DUMP) {
        printf_log(LOG_DUMP, "(short) Dump of Entry point\n");
        int sz = 64;
        uintptr_t lastbyte = GetLastByte(h);
        if (ep + sz >  lastbyte)
            sz = lastbyte - ep;
        DumpBinary((char*)ep, sz);
    }
    /*
    // but instead of regular entrypoint, lets grab "main", it will be easier to manage I guess
    uintptr_t m = FindSymbol(maplib, "main");
    if(m) {
        ep = m;
        printf_log(LOG_DEBUG, "Using \"main\" as Entry Point @%p\n", ep);
        if(box86_log>=LOG_DUMP) {
            printf_log(LOG_DUMP, "(short) Dump of Entry point\n");
            int sz = 64;
            uintptr_t lastbyte = GetLastByte(h);
            if (ep + sz >  lastbyte)
                sz = lastbyte - ep;
            DumpBinary((char*)ep, sz);
        }
    }
    */
    return ep;
}

uintptr_t GetLastByte(elfheader_t* h)
{
    return (uintptr_t)h->memory/* + h->delta*/ + h->memsz;
}

void AddSymbols(lib_t *maplib, kh_mapsymbols_t* mapsymbols, kh_mapsymbols_t* weaksymbols, kh_mapsymbols_t* localsymbols, elfheader_t* h)
{
    printf_log(LOG_DUMP, "Will look for Symbol to add in SymTable(%d)\n", h->numSymTab);
    for (int i=0; i<h->numSymTab; ++i) {
        const char * symname = h->StrTab+h->SymTab[i].st_name;
        int bind = ELF32_ST_BIND(h->SymTab[i].st_info);
        int type = ELF32_ST_TYPE(h->SymTab[i].st_info);
        int vis = h->SymTab[i].st_other&0x3;
        if((type==STT_OBJECT || type==STT_FUNC || type==STT_COMMON || type==STT_TLS  || type==STT_NOTYPE) 
        && (vis==STV_DEFAULT || vis==STV_PROTECTED) && (h->SymTab[i].st_shndx!=0)) {
            if((bind==10/*STB_GNU_UNIQUE*/ || bind==STB_GLOBAL) && FindGlobalSymbol(maplib, symname))
                continue;
            uintptr_t offs = (type==STT_TLS)?h->SymTab[i].st_value:(h->SymTab[i].st_value + h->delta);
            uint32_t sz = h->SymTab[i].st_size;
            printf_log(LOG_DUMP, "Adding Symbol(bind=%s) \"%s\" with offset=%p sz=%d\n", (bind==STB_LOCAL)?"LOCAL":((bind==STB_WEAK)?"WEAK":"GLOBAL"), symname, (void*)offs, sz);
            if(bind==STB_LOCAL)
                AddSymbol(localsymbols, symname, offs, sz);
            else    // add in local and global map 
                if(bind==STB_WEAK) {
                    AddSymbol(weaksymbols, symname, offs, sz);
                } else {
                    AddSymbol(mapsymbols, symname, offs, sz);
                }
        }
    }
    
    printf_log(LOG_DUMP, "Will look for Symbol to add in DynSym (%d)\n", h->numDynSym);
    for (int i=0; i<h->numDynSym; ++i) {
        const char * symname = h->DynStr+h->DynSym[i].st_name;
        int bind = ELF32_ST_BIND(h->DynSym[i].st_info);
        int type = ELF32_ST_TYPE(h->DynSym[i].st_info);
        int vis = h->DynSym[i].st_other&0x3;
        //st_shndx==65521 means ABS value
        if((type==STT_OBJECT || type==STT_FUNC || type==STT_COMMON || type==STT_TLS  || type==STT_NOTYPE) 
        && (vis==STV_DEFAULT || vis==STV_PROTECTED) && (h->DynSym[i].st_shndx!=0 && h->DynSym[i].st_shndx<=65521)) {
            if((bind==10/*STB_GNU_UNIQUE*/ || bind==STB_GLOBAL) && FindGlobalSymbol(maplib, symname))
                continue;
            uintptr_t offs = (type==STT_TLS)?h->DynSym[i].st_value:(h->DynSym[i].st_value + h->delta);
            uint32_t sz = h->DynSym[i].st_size;
            printf_log(LOG_DUMP, "Adding Symbol(bind=%s) \"%s\" with offset=%p sz=%d\n", (bind==STB_LOCAL)?"LOCAL":((bind==STB_WEAK)?"WEAK":"GLOBAL"), symname, (void*)offs, sz);
            if(bind==STB_LOCAL)
                AddSymbol(localsymbols, symname, offs, sz);
            else // add in local and global map 
                if(bind==STB_WEAK) {
                    AddSymbol(weaksymbols, symname, offs, sz);
                } else {
                    AddSymbol(mapsymbols, symname, offs, sz);
                }
        }
    }
    
}

/*
$ORIGIN – Provides the directory the object was loaded from. This token is typical
used for locating dependencies in unbundled packages. For more details of this
token expansion, see “Locating Associated Dependencies”
$OSNAME – Expands to the name of the operating system (see the uname(1) man
page description of the -s option). For more details of this token expansion, see
“System Specific Shared Objects”
$OSREL – Expands to the operating system release level (see the uname(1) man
page description of the -r option). For more details of this token expansion, see
“System Specific Shared Objects”
$PLATFORM – Expands to the processor type of the current machine (see the
uname(1) man page description of the -i option). For more details of this token
expansion, see “System Specific Shared Objects”
*/
int LoadNeededLibs(elfheader_t* h, lib_t *maplib, needed_libs_t* neededlibs, int local, box86context_t *box86, x86emu_t* emu)
{
    DumpDynamicRPath(h);
    // update RPATH first
    for (int i=0; i<h->numDynamic; ++i)
        if(h->Dynamic[i].d_tag==DT_RPATH || h->Dynamic[i].d_tag==DT_RUNPATH) {
            char *rpathref = h->DynStrTab+h->delta+h->Dynamic[i].d_un.d_val;
            char* rpath = rpathref;
            while(strstr(rpath, "$ORIGIN")) {
                char* origin = strdup(h->path);
                char* p = strrchr(origin, '/');
                if(p) *p = '\0';    // remove file name to have only full path, without last '/'
                char* tmp = (char*)calloc(1, strlen(rpath)-strlen("$ORIGIN")+strlen(origin)+1);
                p = strstr(rpath, "$ORIGIN");
                memcpy(tmp, rpath, p-rpath);
                strcat(tmp, origin);
                strcat(tmp, p+strlen("$ORIGIN"));
                if(rpath!=rpathref)
                    free(rpath);
                rpath = tmp;
                free(origin);
            }
            while(strstr(rpath, "${ORIGIN}")) {
                char* origin = strdup(h->path);
                char* p = strrchr(origin, '/');
                if(p) *p = '\0';    // remove file name to have only full path, without last '/'
                char* tmp = (char*)calloc(1, strlen(rpath)-strlen("${ORIGIN}")+strlen(origin)+1);
                p = strstr(rpath, "${ORIGIN}");
                memcpy(tmp, rpath, p-rpath);
                strcat(tmp, origin);
                strcat(tmp, p+strlen("${ORIGIN}"));
                if(rpath!=rpathref)
                    free(rpath);
                rpath = tmp;
                free(origin);
            }
            if(strchr(rpath, '$')) {
                printf_log(LOG_INFO, "BOX86: Warning, RPATH with $ variable not supported yet (%s)\n", rpath);
            } else {
                printf_log(LOG_DEBUG, "Prepending path \"%s\" to BOX86_LD_LIBRARY_PATH\n", rpath);
                PrependList(&box86->box86_ld_lib, rpath, 1);
            }
            if(rpath!=rpathref)
                free(rpath);
        }

    if(!h->neededlibs && neededlibs)
        h->neededlibs = neededlibs;

    DumpDynamicNeeded(h);
    for (int i=0; i<h->numDynamic; ++i)
        if(h->Dynamic[i].d_tag==DT_NEEDED) {
            char *needed = h->DynStrTab+h->delta+h->Dynamic[i].d_un.d_val;
            // TODO: Add LD_LIBRARY_PATH and RPATH Handling
            if(AddNeededLib(maplib, neededlibs, local, needed, box86, emu)) {
                printf_log(LOG_INFO, "Error loading needed lib: \"%s\"\n", needed);
                if(!allow_missing_libs)
                    return 1;   //error...
            }
        }
    return 0;
}

int ElfCheckIfUseTCMallocMinimal(elfheader_t* h)
{
    if(!h)
        return 0;
    for (int i=0; i<h->numDynamic; ++i)
        if(h->Dynamic[i].d_tag==DT_NEEDED) {
            char *needed = h->DynStrTab+h->delta+h->Dynamic[i].d_un.d_val;
            if(!strcmp(needed, "libtcmalloc_minimal.so.4")) // tcmalloc needs to be the 1st lib loaded
                return 1;
            else
                return 0;
        }
    return 0;
}

void RunElfInit(elfheader_t* h, x86emu_t *emu)
{
    if(!h || h->init_done)
        return;
    // reset Segs Cache
    memset(emu->segs_serial, 0, sizeof(emu->segs_serial));
    uintptr_t p = h->initentry + h->delta;
    box86context_t* context = GetEmuContext(emu);
    if(context->deferedInit) {
        if(context->deferedInitSz==context->deferedInitCap) {
            context->deferedInitCap += 4;
            context->deferedInitList = (elfheader_t**)realloc(context->deferedInitList, context->deferedInitCap*sizeof(elfheader_t*));
        }
        context->deferedInitList[context->deferedInitSz++] = h;
        return;
    }
    printf_log(LOG_DEBUG, "Calling Init for %s @%p\n", ElfName(h), (void*)p);
    if(h->initentry)
        RunFunctionWithEmu(emu, 0, p, 3, context->argc, context->argv, context->envv);
    printf_log(LOG_DEBUG, "Done Init for %s\n", ElfName(h));
    // and check init array now
    Elf32_Addr *addr = (Elf32_Addr*)(h->initarray + h->delta);
    for (int i=0; i<h->initarray_sz; ++i) {
        printf_log(LOG_DEBUG, "Calling Init[%d] for %s @%p\n", i, ElfName(h), (void*)addr[i]);
        RunFunctionWithEmu(emu, 0, (uintptr_t)addr[i], 3, context->argc, context->argv, context->envv);
    }

    h->init_done = 1;
    h->fini_done = 0;   // can be fini'd now (in case it was re-inited)
    printf_log(LOG_DEBUG, "All Init Done for %s\n", ElfName(h));
    return;
}

EXPORTDYN
void RunDeferedElfInit(x86emu_t *emu)
{
    box86context_t* context = GetEmuContext(emu);
    if(!context->deferedInit)
        return;
    context->deferedInit = 0;
    if(!context->deferedInitList)
        return;
    for (int i=0; i<context->deferedInitSz; ++i)
        RunElfInit(context->deferedInitList[i], emu);
    free(context->deferedInitList);
    context->deferedInitList = NULL;
    context->deferedInitCap = context->deferedInitSz = 0;
}

void RunElfFini(elfheader_t* h, x86emu_t *emu)
{
    if(!h || h->fini_done)
        return;
    h->fini_done = 1;
    // first check fini array
    Elf32_Addr *addr = (Elf32_Addr*)(h->finiarray + h->delta);
    for (int i=0; i<h->finiarray_sz; ++i) {
        printf_log(LOG_DEBUG, "Calling Fini[%d] for %s @%p\n", i, ElfName(h), (void*)addr[i]);
        RunFunctionWithEmu(emu, 0, (uintptr_t)addr[i], 0);
    }
    // then the "old-style" fini
    if(h->finientry) {
        uintptr_t p = h->finientry + h->delta;
        printf_log(LOG_DEBUG, "Calling Fini for %s @%p\n", ElfName(h), (void*)p);
        RunFunctionWithEmu(emu, 0, p, 0);
    }
    h->init_done = 0;   // can be re-inited again...
    return;
}

uintptr_t GetElfInit(elfheader_t* h)
{
    return h->initentry + h->delta;
}
uintptr_t GetElfFini(elfheader_t* h)
{
    return h->finientry + h->delta;
}

void* GetBaseAddress(elfheader_t* h)
{
    return h->memory;
}

void* GetElfDelta(elfheader_t* h)
{
    return (void*)h->delta;
}

uint32_t GetBaseSize(elfheader_t* h)
{
    return h->memsz;
}

int IsAddressInElfSpace(elfheader_t* h, uintptr_t addr)
{
    if(!h)
        return 0;
    for(int i=0; i<h->multiblock_n; ++i) {
        uintptr_t base = h->multiblock_offs[i];
        uintptr_t end = h->multiblock_offs[i] + h->multiblock_size[i] - 1;
        if(addr>=base && addr<=end)
            return 1;
        
    }
    return 0;
}
elfheader_t* FindElfAddress(box86context_t *context, uintptr_t addr)
{
    for (int i=0; i<context->elfsize; ++i)
        if(IsAddressInElfSpace(context->elfs[i], addr))
            return context->elfs[i];
    
    return NULL;
}

const char* FindNearestSymbolName(elfheader_t* h, void* p, uintptr_t* start, uint32_t* sz)
{
    uintptr_t addr = (uintptr_t)p;

    uint32_t distance = 0x7fffffff;
    const char* ret = NULL;
    uintptr_t s = 0;
    uint32_t size = 0;
    if(!h)
        return ret;

    for (int i=0; i<h->numSymTab && distance!=0; ++i) {   
        const char * symname = h->StrTab+h->SymTab[i].st_name;
        uintptr_t offs = h->SymTab[i].st_value + h->delta;

        if(offs<=addr) {
            if(distance>addr-offs) {
                distance = addr-offs;
                ret = symname;
                s = offs;
                size = h->SymTab[i].st_size;
            }
        }
    }
    for (int i=0; i<h->numDynSym && distance!=0; ++i) {   
        const char * symname = h->DynStr+h->DynSym[i].st_name;
        uintptr_t offs = h->DynSym[i].st_value + h->delta;

        if(offs<=addr) {
            if(distance>addr-offs) {
                distance = addr-offs;
                ret = symname;
                s = offs;
                size = h->DynSym[i].st_size;
            }
        }
    }

    if(start)
        *start = s;
    if(sz)
        *sz = size;

    return ret;
}

void* GetDTatOffset(box86context_t* context, int index, int offset)
{
    return (void*)((char*)GetTLSPointer(context, context->elfs[index])+offset);
}

int32_t GetTLSBase(elfheader_t* h)
{
    return h->tlsbase;
}

uint32_t GetTLSSize(elfheader_t* h)
{
    return h->tlssize;
}

void* GetTLSPointer(box86context_t* context, elfheader_t* h)
{
    if(!h->tlssize)
        return NULL;
    tlsdatasize_t* ptr;
    if ((ptr = (tlsdatasize_t*)pthread_getspecific(context->tlskey)) == NULL) {
        ptr = (tlsdatasize_t*)fillTLSData(context);
    }
    if(ptr->tlssize != context->tlssize)
        ptr = (tlsdatasize_t*)resizeTLSData(context, ptr);
    return ptr->tlsdata+(ptr->tlssize+h->tlsbase);
}

#ifdef DYNAREC
dynablocklist_t* GetDynablocksFromAddress(box86context_t *context, uintptr_t addr)
{
    // if we are here, the there is not block in standard "space"
    /*dynablocklist_t* ret = getDBFromAddress(addr);
    if(ret) {
        return ret;
    }*/
    if(box86_dynarec_forced) {
        addDBFromAddressRange(addr, 1);
        return getDB(addr>>DYNAMAP_SHIFT);
    }
    //check if address is in an elf... if yes, grant a block (should I warn)
    Dl_info info;
    if(dladdr((void*)addr, &info)) {
        dynarec_log(LOG_INFO, "Address %p is in a native Elf memory space (function \"%s\" in %s)\n", (void*)addr, info.dli_sname, info.dli_fname);
        return NULL;
    }
    dynarec_log(LOG_INFO, "Address %p not found in Elf memory and is not a native call wrapper\n", (void*)addr);
    return NULL;
}
#endif

typedef struct my_dl_phdr_info_s {
    void*           dlpi_addr;
    const char*     dlpi_name;
    Elf32_Phdr*     dlpi_phdr;
    int             dlpi_phnum;
} my_dl_phdr_info_t;

static int dl_iterate_phdr_callback(x86emu_t *emu, void* F, my_dl_phdr_info_t *info, size_t size, void* data)
{
    int ret = RunFunctionWithEmu(emu, 0, (uintptr_t)F, 3, info, size, data);
    return ret;
}

#define SUPER() \
GO(0)   \
GO(1)   \
GO(2)   \
GO(3)   \
GO(4)

// dl_iterate_phdr ...
#define GO(A)   \
static uintptr_t my_dl_iterate_phdr_fct_##A = 0;                            \
static int my_dl_iterate_phdr_##A(struct dl_phdr_info* a, size_t b, void* c)\
{                                                                           \
    if(!a->dlpi_name)                                                       \
        return 0;                                                           \
    if(!a->dlpi_name[0]) /*don't send informations about box86 itself*/     \
        return 0;                                                           \
    return RunFunction(my_context, my_dl_iterate_phdr_fct_##A, 3, a, b, c); \
}
SUPER()
#undef GO
static void* find_dl_iterate_phdr_Fct(void* fct)
{
    if(!fct) return fct;
    if(GetNativeFnc((uintptr_t)fct))  return GetNativeFnc((uintptr_t)fct);
    #define GO(A) if(my_dl_iterate_phdr_fct_##A == (uintptr_t)fct) return my_dl_iterate_phdr_##A;
    SUPER()
    #undef GO
    #define GO(A) if(my_dl_iterate_phdr_fct_##A == 0) {my_dl_iterate_phdr_fct_##A = (uintptr_t)fct; return my_dl_iterate_phdr_##A; }
    SUPER()
    #undef GO
    printf_log(LOG_NONE, "Warning, no more slot for elfloader dl_iterate_phdr callback\n");
    return NULL;
}
#undef SUPER

EXPORT int my_dl_iterate_phdr(x86emu_t *emu, void* F, void *data) {
    printf_log(LOG_INFO, "Warning: call to partially implemented dl_iterate_phdr(%p, %p)\n", F, data);
    box86context_t *context = GetEmuContext(emu);
    const char* empty = "";
    int ret = 0;
    for (int idx=0; idx<context->elfsize; ++idx) {
        my_dl_phdr_info_t info;
        info.dlpi_addr = GetElfDelta(context->elfs[idx]);
        info.dlpi_name = idx?context->elfs[idx]->name:empty;    //1st elf is program, and this one doesn't get a name
        info.dlpi_phdr = context->elfs[idx]->PHEntries;
        info.dlpi_phnum = context->elfs[idx]->numPHEntries;
        if((ret = dl_iterate_phdr_callback(emu, F, &info, sizeof(info), data))) {
            return ret;
        }
    }
    // and now, go on native version
    ret = dl_iterate_phdr(find_dl_iterate_phdr_Fct(F), data);
    return ret;
}

void ResetSpecialCaseMainElf(elfheader_t* h)
{
    Elf32_Sym *sym = NULL;
     for (int i=0; i<h->numDynSym; ++i) {
        if(h->DynSym[i].st_info == 17) {
            sym = h->DynSym+i;
            const char * symname = h->DynStr+sym->st_name;
            if(strcmp(symname, "_IO_2_1_stderr_")==0 && ((void*)sym->st_value+h->delta)) {
                memcpy((void*)sym->st_value+h->delta, stderr, sym->st_size);
                my__IO_2_1_stderr_ = (void*)sym->st_value+h->delta;
                printf_log(LOG_DEBUG, "BOX86: Set @_IO_2_1_stderr_ to %p\n", my__IO_2_1_stderr_);
            } else
            if(strcmp(symname, "_IO_2_1_stdin_")==0 && ((void*)sym->st_value+h->delta)) {
                memcpy((void*)sym->st_value+h->delta, stdin, sym->st_size);
                my__IO_2_1_stdin_ = (void*)sym->st_value+h->delta;
                printf_log(LOG_DEBUG, "BOX86: Set @_IO_2_1_stdin_ to %p\n", my__IO_2_1_stdin_);
            } else
            if(strcmp(symname, "_IO_2_1_stdout_")==0 && ((void*)sym->st_value+h->delta)) {
                memcpy((void*)sym->st_value+h->delta, stdout, sym->st_size);
                my__IO_2_1_stdout_ = (void*)sym->st_value+h->delta;
                printf_log(LOG_DEBUG, "BOX86: Set @_IO_2_1_stdout_ to %p\n", my__IO_2_1_stdout_);
            }
        }
    }
}


void CreateMemorymapFile(box86context_t* context, int fd)
{
    char buff[1024];
    struct stat st;
    int dummy;
    (void)dummy;

    elfheader_t *h = context->elfs[0];

    if (stat(h->path, &st)) {
        printf_log(LOG_INFO, "Failed to stat file %s (creating memory maps \"file\")!", h->path);
        // Some constants, to have "valid" values
        st.st_dev = makedev(0x03, 0x00);
        st.st_ino = 0;
    }

    for (int i=0; i<h->numPHEntries; ++i) {
        if (h->PHEntries[i].p_memsz == 0) continue;

        sprintf(buff, "%08x-%08x %c%c%c%c %08x %02x:%02x %ld %s\n", (uintptr_t)h->PHEntries[i].p_vaddr + h->delta,
            (uintptr_t)h->PHEntries[i].p_vaddr + h->PHEntries[i].p_memsz + h->delta,
            (h->PHEntries[i].p_type & (PF_R|PF_X) ? 'r':'-'), (h->PHEntries[i].p_type & PF_W ? 'w':'-'),
            (h->PHEntries[i].p_type & PF_X ? 'x':'-'), 'p', // p for private or s for shared
            (uintptr_t)h->PHEntries[i].p_offset,
            major(st.st_dev), minor(st.st_dev), st.st_ino, h->path);
        
        dummy = write(fd, buff, strlen(buff));
    }
}

void ElfAttachLib(elfheader_t* head, library_t* lib)
{
    if(!head)
        return;
    head->lib = lib;
}

uintptr_t pltResolver = ~0;
EXPORT void PltResolver(x86emu_t* emu)
{
    uintptr_t addr = Pop32(emu);
    int slot = (int)Pop32(emu);
    elfheader_t *h = (elfheader_t*)addr;
    printf_log(LOG_DEBUG, "PltResolver: Addr=%p, Slot=%d Return=%p: elf is %s\n", (void*)addr, slot, *(void**)(R_ESP), h->name);

    Elf32_Rel * rel = (Elf32_Rel *)(h->jmprel + h->delta + slot);

    Elf32_Sym *sym = &h->DynSym[ELF32_R_SYM(rel->r_info)];
    int bind = ELF32_ST_BIND(sym->st_info);
    const char* symname = SymName(h, sym);
    uint32_t *p = (uint32_t*)(rel->r_offset + h->delta);
    uintptr_t offs = 0;
    uintptr_t end = 0;

    library_t* lib = h->lib;
    lib_t* local_maplib = GetMaplib(lib);
    if(local_maplib)
        GetGlobalSymbolStartEnd(local_maplib, symname, &offs, &end);
    if(!offs && !end)
        GetGlobalSymbolStartEnd(my_context->maplib, symname, &offs, &end);

    if (!offs) {
        printf_log(LOG_NONE, "Error: PltReolver: Symbol %s not found, cannot apply R_386_JMP_SLOT @%p (%p) in %s\n", symname, p, *(void**)p, h->name);
        emu->quit = 1;
        return;
    } else {
        if(p) {
            printf_log(LOG_DEBUG, "PltReolver: Apply %s R_386_JMP_SLOT @%p with sym=%s (%p -> %p / %s)\n", (bind==STB_LOCAL)?"Local":"Global", p, symname, *(void**)p, (void*)offs, ElfName(FindElfAddress(my_context, offs)));
            *p = offs;
        } else {
            printf_log(LOG_NONE, "PltReolver: Warning, Symbol %s found, but Jump Slot Offset is NULL \n", symname);
        }
    }

    // jmp to function
    R_EIP = offs;
}
