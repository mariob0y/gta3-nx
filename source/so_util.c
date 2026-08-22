/* so_util.c -- utils to load and hook .so modules
 *
 * Copyright (C) 2021 Andy Nguyen, fgsfds
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <switch.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <elf.h>

#include "config.h"
#include "so_util.h"
#include "util.h"
#include "error.h"

#ifndef DT_RELR
#define DT_RELR    36
#define DT_RELRSZ  35
#define DT_RELRENT 37
#endif
#define DT_ANDROID_RELR    0x6fffe000
#define DT_ANDROID_RELRSZ  0x6fffe001
#define DT_ANDROID_RELRENT 0x6fffe003

static so_module *so_list = NULL;

#define MAX_HOOKS 256
static struct { uintptr_t addr; uintptr_t rw_addr; } g_applied_hooks[MAX_HOOKS];
static int g_num_applied_hooks = 0;

void hook_arm64(uintptr_t addr, uintptr_t dst) {
  if (addr == 0)
    return;

  uintptr_t rw_addr = addr;
  for (so_module *m = so_list; m != NULL; m = m->next) {
    uintptr_t vstart = (uintptr_t)m->load_virtbase;
    uintptr_t vend   = vstart + m->load_size;
    uintptr_t bstart = (uintptr_t)m->load_base;
    uintptr_t bend   = bstart + m->load_size;

    if (addr >= vstart && addr < vend) {
      rw_addr = (addr - vstart) + bstart;
      break;
    } else if (addr >= bstart && addr < bend) {
      rw_addr = addr;
      break;
    }
  }

  for (int i = 0; i < g_num_applied_hooks; i++) {
    uintptr_t prev_rw = g_applied_hooks[i].rw_addr;
    if ((rw_addr >= prev_rw && rw_addr < prev_rw + 16) ||
        (prev_rw >= rw_addr && prev_rw < rw_addr + 16)) {
      fatal_error("[HOOK CRITICAL] ARM64 hook at %p (rw %p) collides with hook #%d at rw %p (trampolines are 16B)\n",
                  (void *)addr, (void *)rw_addr, i, (void *)prev_rw);
    }
  }

  if (g_num_applied_hooks < MAX_HOOKS) {
    g_applied_hooks[g_num_applied_hooks].addr = addr;
    g_applied_hooks[g_num_applied_hooks].rw_addr = rw_addr;
    g_num_applied_hooks++;
  }

  debugPrintf("[HOOK] Applying ARM64 hook at %p (rw %p) -> dst %p\n", (void *)addr, (void *)rw_addr, (void *)dst);

  uint32_t *hook = (uint32_t *)rw_addr;
  hook[0] = 0x58000051u; // LDR X17, #0x8
  hook[1] = 0xd61f0220u; // BR X17
  *(uint64_t *)(hook + 2) = dst;
}

void so_flush_caches(so_module *mod) {
  debugPrintf("[LOADER] Flushing I/D cache for module %s at %p (size=%zu)\n", mod->name, mod->load_virtbase, mod->load_size);
  armDCacheFlush(mod->load_virtbase, mod->load_size);
  armICacheInvalidate(mod->load_virtbase, mod->load_size);
}

void so_free_temp(so_module *mod) {
  if (mod->so_base) {
    free(mod->so_base);
    mod->so_base = NULL;
    debugPrintf("[LOADER] Freed temporary file buffer for module %s\n", mod->name);
  }
}

void so_finalize(so_module *mod) {
  Result rc = 0;
  debugPrintf("[LOADER] Finalizing memory layout for module %s (virtbase=%p, size=%zu)\n", mod->name, mod->load_virtbase, mod->load_size);

  rc = svcMapProcessCodeMemory(envGetOwnProcessHandle(), (u64)mod->load_virtbase, (u64)mod->load_base, mod->load_size);
  if (R_FAILED(rc)) fatal_error("Error: svcMapProcessCodeMemory failed:\n0x%08x", rc);

  // Update syms and dynstrtab pointers to point into load_virtbase space,
  // since load_base becomes inaccessible (Perm_None) after svcMapProcessCodeMemory.
  if (mod->load_virtbase && mod->load_base) {
    uintptr_t delta = (uintptr_t)mod->load_virtbase - (uintptr_t)mod->load_base;
    if (mod->syms) {
      mod->syms = (Elf64_Sym *)((uintptr_t)mod->syms + delta);
    }
    if (mod->dynstrtab) {
      mod->dynstrtab = (char *)((uintptr_t)mod->dynstrtab + delta);
    }
  }

  const size_t num_pages = mod->load_size / 0x1000;
  uint8_t *is_x_page = calloc(num_pages, 1);
  if (!is_x_page) fatal_error("Error: out of memory in so_finalize");

  for (int i = 0; i < mod->phnum; i++) {
    const Elf64_Phdr *p = &mod->phdr[i];
    if (p->p_type != PT_LOAD || (p->p_flags & PF_X) != PF_X)
      continue;
    const size_t first = p->p_vaddr / 0x1000;
    const size_t last = (ALIGN_MEM(p->p_vaddr + p->p_memsz, 0x1000) / 0x1000) - 1;
    for (size_t pg = first; pg <= last && pg < num_pages; pg++)
      is_x_page[pg] = 1;
  }

  // Include dynamic trampoline tail pages in executable page set before applying permissions
  if (mod->tramp_offset > 0 && mod->load_size > mod->tramp_offset) {
    const size_t first_tramp_pg = mod->tramp_offset / 0x1000;
    const size_t last_tramp_pg = (ALIGN_MEM(mod->load_size, 0x1000) / 0x1000) - 1;
    for (size_t pg = first_tramp_pg; pg <= last_tramp_pg && pg < num_pages; pg++)
      is_x_page[pg] = 1;
  }

  for (int want_x = 1; want_x >= 0; want_x--) {
    size_t pg = 0;
    while (pg < num_pages) {
      if (is_x_page[pg] != want_x) {
        pg++;
        continue;
      }
      size_t run_end = pg;
      while (run_end < num_pages && is_x_page[run_end] == want_x)
        run_end++;
      const u64 addr = (u64)mod->load_virtbase + pg * 0x1000;
      const u64 size = (run_end - pg) * 0x1000;
      rc = svcSetProcessMemoryPermission(envGetOwnProcessHandle(), addr, size, want_x ? Perm_Rx : Perm_Rw);
      if (R_FAILED(rc)) fatal_error("Error: could not map %u bytes of %s memory at %p:\n0x%08x", (u32)size, want_x ? "RX" : "RW", (void *)addr, rc);
      pg = run_end;
    }
  }

  free(is_x_page);
  debugPrintf("[LOADER] Module %s final permissions set successfully\n", mod->name);
}

int so_load(so_module *mod, const char *filename, void *base, size_t max_size) {
  int res = 0;
  debugPrintf("[LOADER] Loading shared object %s...\n", filename);

  memset(mod, 0, sizeof(*mod));
  strncpy(mod->name, filename, sizeof(mod->name) - 1);

  FILE *fd = fopen(filename, "rb");
  if (fd == NULL) {
    debugPrintf("[LOADER] ERROR: Could not open %s\n", filename);
    return -1;
  }

  fseek(fd, 0, SEEK_END);
  mod->so_size = ftell(fd);
  fseek(fd, 0, SEEK_SET);

  debugPrintf("[LOADER] Reading %s (%zu bytes)\n", filename, mod->so_size);
  mod->so_base = malloc(mod->so_size);
  if (!mod->so_base) {
    fclose(fd);
    debugPrintf("[LOADER] ERROR: Out of memory reading %s\n", filename);
    return -2;
  }

  fread(mod->so_base, mod->so_size, 1, fd);
  fclose(fd);

  if (memcmp(mod->so_base, ELFMAG, SELFMAG) != 0) {
    debugPrintf("[LOADER] ERROR: Invalid ELF magic in %s\n", filename);
    res = -1;
    goto err_free_so;
  }

  mod->elf_hdr = (Elf64_Ehdr *)mod->so_base;
  mod->prog_hdr = (Elf64_Phdr *)((uintptr_t)mod->so_base + mod->elf_hdr->e_phoff);
  mod->sec_hdr = (Elf64_Shdr *)((uintptr_t)mod->so_base + mod->elf_hdr->e_shoff);
  mod->shstrtab = (char *)((uintptr_t)mod->so_base + mod->sec_hdr[mod->elf_hdr->e_shstrndx].sh_offset);

  if (mod->elf_hdr->e_phnum > SO_MAX_SEGMENTS * 2) {
    debugPrintf("[LOADER] ERROR: %s has too many program headers (%d)\n", filename, mod->elf_hdr->e_phnum);
    res = -4;
    goto err_free_so;
  }

  mod->phnum = mod->elf_hdr->e_phnum;
  memcpy(mod->phdr, mod->prog_hdr, mod->phnum * sizeof(Elf64_Phdr));

  mod->image_size = 0;
  for (int i = 0; i < mod->elf_hdr->e_phnum; i++) {
    if (mod->prog_hdr[i].p_type == PT_LOAD) {
      const size_t seg_end = mod->prog_hdr[i].p_vaddr + mod->prog_hdr[i].p_memsz;
      if (seg_end > mod->image_size)
        mod->image_size = seg_end;
    }
  }

  mod->image_size = ALIGN_MEM(mod->image_size, 0x1000);
  mod->tramp_offset = mod->image_size;
  mod->tramp_used = 0;
  mod->load_size = mod->tramp_offset + SO_TRAMPOLINE_SIZE;
  if (mod->load_size > max_size) {
    debugPrintf("[LOADER] ERROR: %s load size (%zu KB) exceeds max available (%zu KB)\n", filename, mod->load_size >> 10, max_size >> 10);
    res = -3;
    goto err_free_so;
  }

  mod->load_base = base;
  if (!mod->load_base) goto err_free_so;
  memset(mod->load_base, 0, mod->load_size);

  virtmemLock();
  mod->load_virtbase = virtmemFindCodeMemory(mod->load_size, 0x1000);
  mod->load_memrv = virtmemAddReservation(mod->load_virtbase, mod->load_size);
  virtmemUnlock();

  debugPrintf("[LOADER] %s: load base = %p, virtbase = %p, size = %u KB\n", filename, mod->load_base, mod->load_virtbase, (u32)(mod->load_size / 1024));

  for (int i = 0; i < mod->elf_hdr->e_phnum; i++) {
    Elf64_Phdr *p = &mod->prog_hdr[i];
    if (p->p_type == PT_LOAD) {
      memcpy((void *)((uintptr_t)mod->load_base + p->p_vaddr),
             (void *)((uintptr_t)mod->so_base + p->p_offset),
             p->p_filesz);
    }
    p->p_vaddr += (Elf64_Addr)mod->load_virtbase;
  }

  mod->syms = NULL;
  mod->dynstrtab = NULL;

  for (int i = 0; i < mod->elf_hdr->e_shnum; i++) {
    char *sh_name = mod->shstrtab + mod->sec_hdr[i].sh_name;
    if (strcmp(sh_name, ".dynsym") == 0) {
      mod->syms = (Elf64_Sym *)((uintptr_t)mod->load_base + mod->sec_hdr[i].sh_addr);
      mod->num_syms = mod->sec_hdr[i].sh_size / sizeof(Elf64_Sym);
    } else if (strcmp(sh_name, ".dynstr") == 0) {
      mod->dynstrtab = (char *)((uintptr_t)mod->load_base + mod->sec_hdr[i].sh_addr);
    }
  }

  if (mod->syms == NULL || mod->dynstrtab == NULL) {
    debugPrintf("[LOADER] ERROR: %s missing .dynsym or .dynstr section!\n", filename);
    res = -2;
    goto err_free_load;
  }

  mod->next = NULL;
  if (!so_list) {
    so_list = mod;
  } else {
    so_module *m = so_list;
    while (m->next) m = m->next;
    m->next = mod;
  }

  debugPrintf("[LOADER] Module %s successfully loaded (%d dynamic symbols)\n", filename, mod->num_syms);
  return 0;

err_free_load:
  virtmemLock();
  virtmemRemoveReservation(mod->load_memrv);
  virtmemUnlock();
err_free_so:
  free(mod->so_base);
  mod->so_base = NULL;

  return res;
}

static Elf64_Xword so_dynamic_tag(so_module *mod, Elf64_Sxword tag) {
  for (int i = 0; i < mod->phnum; i++) {
    if (mod->phdr[i].p_type == PT_DYNAMIC) {
      const Elf64_Dyn *dyn = (const Elf64_Dyn *)((uintptr_t)mod->so_base + mod->phdr[i].p_offset);
      for (; dyn->d_tag != DT_NULL; dyn++)
        if (dyn->d_tag == tag)
          return dyn->d_un.d_val;
    }
  }
  return 0;
}

static void so_process_relr(so_module *mod, const Elf64_Xword *relr, size_t relrsz) {
  uintptr_t where = 0;
  const size_t count = relrsz / sizeof(Elf64_Xword);
  for (size_t i = 0; i < count; i++) {
    const Elf64_Xword entry = relr[i];
    if ((entry & 1) == 0) {
      where = (uintptr_t)entry;
      *(uint64_t *)((uintptr_t)mod->load_base + where) += (uint64_t)mod->load_virtbase;
      where += 8;
    } else {
      for (int bit = 1; bit < 64; bit++) {
        if (entry & (1ull << bit))
          *(uint64_t *)((uintptr_t)mod->load_base + where + (bit - 1) * 8) += (uint64_t)mod->load_virtbase;
      }
      where += 63 * 8;
    }
  }
}

int so_relocate(so_module *mod) {
  int deferred_abs64 = 0;
  debugPrintf("[RELOC] Relocating module %s...\n", mod->name);

  for (int i = 0; i < mod->elf_hdr->e_shnum; i++) {
    char *sh_name = mod->shstrtab + mod->sec_hdr[i].sh_name;
    if (strcmp(sh_name, ".rela.dyn") == 0 || strcmp(sh_name, ".rela.plt") == 0) {
      Elf64_Rela *rels = (Elf64_Rela *)((uintptr_t)mod->load_base + mod->sec_hdr[i].sh_addr);
      for (int j = 0; j < mod->sec_hdr[i].sh_size / sizeof(Elf64_Rela); j++) {
        uintptr_t *ptr = (uintptr_t *)((uintptr_t)mod->load_base + rels[j].r_offset);
        Elf64_Sym *sym = &mod->syms[ELF64_R_SYM(rels[j].r_info)];

        int type = ELF64_R_TYPE(rels[j].r_info);
        switch (type) {
          case R_AARCH64_ABS64:
            if (sym->st_shndx == SHN_UNDEF) {
              *ptr = rels[j].r_addend;
              deferred_abs64++;
            } else {
              *ptr = (uintptr_t)mod->load_virtbase + sym->st_value + rels[j].r_addend;
            }
            break;

          case R_AARCH64_RELATIVE:
            *ptr = (uintptr_t)mod->load_virtbase + rels[j].r_addend;
            break;

          case R_AARCH64_GLOB_DAT:
          case R_AARCH64_JUMP_SLOT:
          {
            if (sym->st_shndx != SHN_UNDEF)
              *ptr = (uintptr_t)mod->load_virtbase + sym->st_value + rels[j].r_addend;
            break;
          }

          default:
            fatal_error("Error: unknown relocation type:\n%x\n", type);
            break;
        }
      }
    }
  }

  Elf64_Xword relr_off = so_dynamic_tag(mod, DT_RELR);
  Elf64_Xword relr_size = so_dynamic_tag(mod, DT_RELRSZ);
  if (!relr_off) {
    relr_off = so_dynamic_tag(mod, DT_ANDROID_RELR);
    relr_size = so_dynamic_tag(mod, DT_ANDROID_RELRSZ);
  }
  if (relr_off && relr_size) {
    debugPrintf("[RELOC] %s: processing %u bytes of RELR relocations\n", mod->name, (u32)relr_size);
    so_process_relr(mod, (const Elf64_Xword *)((uintptr_t)mod->load_base + relr_off), relr_size);
  }

  if (deferred_abs64)
    debugPrintf("[RELOC] %s: deferred %d ABS64 imports\n", mod->name, deferred_abs64);

  return 0;
}

static uintptr_t so_lookup_export(so_module *mod, const char *name) {
  for (int i = 0; i < mod->num_syms; i++) {
    if (mod->syms[i].st_shndx == SHN_UNDEF)
      continue;
    if (ELF64_ST_BIND(mod->syms[i].st_info) == STB_LOCAL)
      continue;
    const char *sname = mod->dynstrtab + mod->syms[i].st_name;
    if (sname[0] == name[0] && strcmp(sname, name) == 0)
      return (uintptr_t)mod->load_virtbase + mod->syms[i].st_value;
  }
  return 0;
}

static uintptr_t so_resolve_symbol(so_module *mod, DynLibFunction *funcs, int num_funcs, const char *name) {
  for (int k = 0; k < num_funcs; k++) {
    if (strcmp(name, funcs[k].symbol) == 0)
      return funcs[k].func;
  }

  for (so_module *m = so_list; m; m = m->next) {
    if (m == mod)
      continue;
    const uintptr_t addr = so_lookup_export(m, name);
    if (addr)
      return addr;
  }

  return 0;
}

int so_resolve(so_module *mod, DynLibFunction *funcs, int num_funcs, int taint_missing_imports) {
  int missing = 0;
  int resolved_abs64 = 0;
  debugPrintf("[RESOLVE] Resolving symbols for module %s...\n", mod->name);

  for (int i = 0; i < mod->elf_hdr->e_shnum; i++) {
    char *sh_name = mod->shstrtab + mod->sec_hdr[i].sh_name;
    if (strcmp(sh_name, ".rela.dyn") == 0 || strcmp(sh_name, ".rela.plt") == 0) {
      Elf64_Rela *rels = (Elf64_Rela *)((uintptr_t)mod->load_base + mod->sec_hdr[i].sh_addr);
      for (int j = 0; j < mod->sec_hdr[i].sh_size / sizeof(Elf64_Rela); j++) {
        uintptr_t *ptr = (uintptr_t *)((uintptr_t)mod->load_base + rels[j].r_offset);
        Elf64_Sym *sym = &mod->syms[ELF64_R_SYM(rels[j].r_info)];

        int type = ELF64_R_TYPE(rels[j].r_info);
        switch (type) {
          case R_AARCH64_ABS64:
          case R_AARCH64_GLOB_DAT:
          case R_AARCH64_JUMP_SLOT:
          {
            if (sym->st_shndx == SHN_UNDEF) {
              char *name = mod->dynstrtab + sym->st_name;
              uintptr_t addr = so_resolve_symbol(mod, funcs, num_funcs, name);
              if (addr) {
                *ptr = addr + rels[j].r_addend;
                if (type == R_AARCH64_ABS64)
                  resolved_abs64++;
              } else {
                missing++;
                debugPrintf("[RESOLVE] %s: unresolved import: %s\n", mod->name, name);
                if (taint_missing_imports)
                  *ptr = rels[j].r_offset;
              }
            }
            break;
          }
          default:
            break;
        }
      }
    }
  }

  if (missing)
    debugPrintf("[RESOLVE] WARNING: %s has %d unresolved imports\n", mod->name, missing);
  if (resolved_abs64)
    debugPrintf("[RESOLVE] %s: resolved %d ABS64 imports\n", mod->name, resolved_abs64);

  return 0;
}

void so_execute_init_array(so_module *mod) {
  debugPrintf("[INIT] Running .init_array static constructors for %s...\n", mod->name);
  for (int i = 0; i < mod->elf_hdr->e_shnum; i++) {
    char *sh_name = mod->shstrtab + mod->sec_hdr[i].sh_name;
    if (strcmp(sh_name, ".init_array") == 0) {
      int (** init_array)() = (void *)((uintptr_t)mod->load_virtbase + mod->sec_hdr[i].sh_addr);
      for (int j = 0; j < mod->sec_hdr[i].sh_size / 8; j++) {
        if (init_array[j] != 0)
          init_array[j]();
      }
    }
  }
  debugPrintf("[INIT] Finished .init_array for %s\n", mod->name);
}

uintptr_t so_find_addr(so_module *mod, const char *symbol) {
  for (int i = 0; i < mod->num_syms; i++) {
    char *name = mod->dynstrtab + mod->syms[i].st_name;
    if (strcmp(name, symbol) == 0)
      return (uintptr_t)mod->load_base + mod->syms[i].st_value;
  }

  fatal_error("Error: could not find symbol:\n%s\n", symbol);
  return 0;
}

uintptr_t so_find_addr_rx(so_module *mod, const char *symbol) {
  const uintptr_t addr = so_try_find_addr_rx(mod, symbol);
  if (!addr)
    fatal_error("Error: could not find symbol:\n%s\n", symbol);
  return addr;
}

uintptr_t so_try_find_addr_rx(so_module *mod, const char *symbol) {
  for (int i = 0; i < mod->num_syms; i++) {
    char *name = mod->dynstrtab + mod->syms[i].st_name;
    if (strcmp(name, symbol) == 0)
      return (uintptr_t)mod->load_virtbase + mod->syms[i].st_value;
  }
  return 0;
}

DynLibFunction *so_find_import(DynLibFunction *funcs, int num_funcs, const char *name) {
  for (int i = 0; i < num_funcs; ++i)
    if (!strcmp(funcs[i].symbol, name))
      return &funcs[i];
  return NULL;
}

int so_unload(so_module *mod) {
  if (mod->load_base == NULL)
    return -1;

  debugPrintf("[LOADER] Unloading module %s...\n", mod->name);

  if (mod->so_base) {
    so_free_temp(mod);
  }

  for (int i = 0; i < mod->phnum; i++) {
    const Elf64_Phdr *p = &mod->phdr[i];
    if (p->p_type != PT_LOAD || !(p->p_flags & PF_X))
      continue;
    const u64 seg_start = ((u64)mod->load_virtbase + p->p_vaddr) & ~0xFFFull;
    const u64 seg_end = ALIGN_MEM((u64)mod->load_virtbase + p->p_vaddr + p->p_memsz, 0x1000);
    svcSetProcessMemoryPermission(envGetOwnProcessHandle(), seg_start, seg_end - seg_start, Perm_Rw);
  }

  svcUnmapProcessCodeMemory(envGetOwnProcessHandle(), (u64)mod->load_virtbase, (u64)mod->load_base, mod->load_size);

  virtmemLock();
  virtmemRemoveReservation(mod->load_memrv);
  virtmemUnlock();

  if (so_list == mod) {
    so_list = mod->next;
  } else {
    for (so_module *m = so_list; m; m = m->next) {
      if (m->next == mod) {
        m->next = mod->next;
        break;
      }
    }
  }

  return 0;
}

uintptr_t so_hook_plt(so_module *mod, const char *symbol, uintptr_t new_dst) {
  if (!mod || !mod->load_base || !mod->sec_hdr || !mod->dynstrtab || !mod->syms) return 0;
  for (int i = 0; i < mod->elf_hdr->e_shnum; i++) {
    char *sh_name = mod->shstrtab + mod->sec_hdr[i].sh_name;
    if (strcmp(sh_name, ".rela.dyn") == 0 || strcmp(sh_name, ".rela.plt") == 0) {
      Elf64_Rela *rels = (Elf64_Rela *)((uintptr_t)mod->load_base + mod->sec_hdr[i].sh_addr);
      for (int j = 0; j < mod->sec_hdr[i].sh_size / sizeof(Elf64_Rela); j++) {
        Elf64_Sym *sym = &mod->syms[ELF64_R_SYM(rels[j].r_info)];
        const char *name = mod->dynstrtab + sym->st_name;
        if (name && strcmp(name, symbol) == 0) {
          uintptr_t *ptr = (uintptr_t *)((uintptr_t)mod->load_base + rels[j].r_offset);
          uintptr_t old = *ptr;
          *ptr = new_dst;
          debugPrintf("[PLT_HOOK] Hooked PLT/GOT entry '%s' at offset 0x%lx (old=%p -> new=%p)\n",
                      symbol, (unsigned long)rels[j].r_offset, (void *)old, (void *)new_dst);
          return old;
        }
      }
    }
  }
  return 0;
}

struct so_dl_phdr_info {
  Elf64_Addr dlpi_addr;
  const char *dlpi_name;
  const Elf64_Phdr *dlpi_phdr;
  Elf64_Half dlpi_phnum;
};

int so_dl_iterate_phdr(int (*callback)(void *info, size_t size, void *data), void *data) {
  int ret = 0;
  for (so_module *mod = so_list; mod; mod = mod->next) {
    struct so_dl_phdr_info info;
    info.dlpi_addr = (Elf64_Addr)mod->load_virtbase;
    info.dlpi_name = mod->name;
    info.dlpi_phdr = mod->phdr;
    info.dlpi_phnum = mod->phnum;
    ret = callback(&info, sizeof(info), data);
    if (ret)
      break;
  }
  return ret;
}

const char *so_nearest_symbol(so_module *mod, uintptr_t offset) {
  if (!mod || !mod->dynstrtab || !mod->syms) return NULL;
  const char *best_name = NULL;
  uintptr_t best_val = 0;
  for (int i = 0; i < mod->num_syms; i++) {
    uintptr_t val = (uintptr_t)mod->syms[i].st_value;
    if (val == 0 || val > offset) continue;
    if (!best_name || val > best_val) {
      best_val = val;
      best_name = mod->dynstrtab + mod->syms[i].st_name;
    }
  }
  return best_name;
}

bool resolve_pc_to_module_and_symbol(uintptr_t pc, char *out_buf, size_t out_size) {
  if (!out_buf || out_size == 0) return false;
  for (so_module *m = so_list; m != NULL; m = m->next) {
    uintptr_t vstart = (uintptr_t)m->load_virtbase;
    uintptr_t vend = vstart + m->load_size;
    if (pc >= vstart && pc < vend) {
      uintptr_t off = pc - vstart;
      const char *sym = so_nearest_symbol(m, off);
      const char *mod_name = m->name[0] ? m->name : "libGame.so";
      if (sym) {
        snprintf(out_buf, out_size, "%s+0x%lx (near %s)", mod_name, (unsigned long)off, sym);
      } else {
        snprintf(out_buf, out_size, "%s+0x%lx", mod_name, (unsigned long)off);
      }
      return true;
    }
  }
  snprintf(out_buf, out_size, "outside loaded modules (PC=0x%lx)", (unsigned long)pc);
  return false;
}

uintptr_t hook_arm64_trampoline(uintptr_t addr, uintptr_t dst) {
  if (addr == 0 || dst == 0)
    return 0;

  if ((addr & 3) != 0) {
    debugPrintf("[HOOK ERROR] Target address %p is not 4-byte aligned\n", (void *)addr);
    return 0;
  }

  so_module *mod = NULL;
  uintptr_t rw_addr = addr;
  for (so_module *m = so_list; m != NULL; m = m->next) {
    uintptr_t vstart = (uintptr_t)m->load_virtbase;
    uintptr_t vend   = vstart + m->load_size;
    uintptr_t bstart = (uintptr_t)m->load_base;
    uintptr_t bend   = bstart + m->load_size;

    if (addr >= vstart && addr < vend) {
      rw_addr = (addr - vstart) + bstart;
      mod = m;
      break;
    } else if (addr >= bstart && addr < bend) {
      rw_addr = addr;
      mod = m;
      break;
    }
  }

  if (!mod) {
    debugPrintf("[HOOK ERROR] Target address %p not found in any loaded module\n", (void *)addr);
    return 0;
  }

  if (mod->tramp_used + TRAMPOLINE_STRIDE > SO_TRAMPOLINE_SIZE) {
    fatal_error("[HOOK ERROR] Module %s trampoline pool exhausted (%zu / %d bytes)\n",
                mod->name, mod->tramp_used, SO_TRAMPOLINE_SIZE);
    return 0;
  }

  uintptr_t tramp_slot_offset = mod->tramp_offset + mod->tramp_used;
  uint32_t *t_rw = (uint32_t *)((uintptr_t)mod->load_base + tramp_slot_offset);
  uintptr_t t_virt = (uintptr_t)mod->load_virtbase + tramp_slot_offset;

  // Inspect and relocate the first 16 bytes (4 instructions)
  const uint32_t *insns = (const uint32_t *)rw_addr;
  for (int i = 0; i < 4; i++) {
    uint32_t insn = insns[i];
    uintptr_t orig_pc = addr + i * 4;
    uintptr_t tramp_pc = t_virt + i * 4;

    // RET (or BR/BLR to a register): if this appears before the full
    // 4-instruction/16-byte relocation window is filled, the function's
    // real body is shorter than 16 bytes -- the remaining slot(s) belong to
    // a DIFFERENT function that happens to follow it in memory, and must
    // not be copied/overwritten as if they were part of this one's
    // prologue. (Found the hard way: hooking a 12-byte function corrupted
    // the start of the next function in memory, causing an Undefined
    // Instruction crash at startup instead of a clean rejection here.)
    if (i < 3 && (insn & 0xfffffc1fu) == 0xd65f0000u) {
      debugPrintf("[HOOK REJECT] Symbol at %p (+%d) returns early (RET, 0x%08x) -- function shorter than 16 bytes, unsafe to hook\n", (void *)addr, i * 4, insn);
      return 0;
    }
    // ADRP: (insn & 0x9f000000u) == 0x90000000u
    if ((insn & 0x9f000000u) == 0x90000000u) {
      uint32_t immlo = (insn >> 29) & 3;
      uint32_t immhi = (insn >> 5) & 0x7ffff;
      int32_t imm21 = (int32_t)((immhi << 2) | immlo);
      if (imm21 & 0x100000) imm21 |= (int32_t)0xffe00000; // Sign-extend 21-bit
      uintptr_t target_page = (orig_pc & ~0xfffull) + ((int64_t)imm21 << 12);
      int64_t new_page_diff = (int64_t)target_page - (int64_t)(tramp_pc & ~0xfffull);
      int64_t new_imm21 = new_page_diff >> 12;
      if (new_imm21 < -0x100000 || new_imm21 > 0xfffff) {
        debugPrintf("[HOOK REJECT] Symbol at %p (+%d) ADRP out of range (diff=%lld)\n", (void *)addr, i * 4, (long long)new_page_diff);
        return 0;
      }
      uint32_t new_immlo = (uint32_t)new_imm21 & 3;
      uint32_t new_immhi = ((uint32_t)new_imm21 >> 2) & 0x7ffff;
      t_rw[i] = (insn & 0x9f00001fu) | (new_immlo << 29) | (new_immhi << 5);
      debugPrintf("[TRAMP_RELOC] Relocated ADRP at %p (+%d) [0x%08x -> 0x%08x, target_page %p]\n",
                  (void *)addr, i * 4, insn, t_rw[i], (void *)target_page);
    }
    // ADR: (insn & 0x9f000000u) == 0x10000000u
    else if ((insn & 0x9f000000u) == 0x10000000u) {
      uint32_t immlo = (insn >> 29) & 3;
      uint32_t immhi = (insn >> 5) & 0x7ffff;
      int32_t imm21 = (int32_t)((immhi << 2) | immlo);
      if (imm21 & 0x100000) imm21 |= (int32_t)0xffe00000;
      uintptr_t target = orig_pc + (int64_t)imm21;
      int64_t new_diff = (int64_t)target - (int64_t)tramp_pc;
      if (new_diff < -0x100000 || new_diff > 0xfffff) {
        debugPrintf("[HOOK REJECT] Symbol at %p (+%d) ADR out of range (diff=%lld)\n", (void *)addr, i * 4, (long long)new_diff);
        return 0;
      }
      uint32_t new_immlo = (uint32_t)new_diff & 3;
      uint32_t new_immhi = ((uint32_t)new_diff >> 2) & 0x7ffff;
      t_rw[i] = (insn & 0x9f00001fu) | (new_immlo << 29) | (new_immhi << 5);
      debugPrintf("[TRAMP_RELOC] Relocated ADR at %p (+%d) [0x%08x -> 0x%08x, target %p]\n",
                  (void *)addr, i * 4, insn, t_rw[i], (void *)target);
    }
    // B or BL: (insn & 0x7c000000u) == 0x14000000u
    else if ((insn & 0x7c000000u) == 0x14000000u) {
      int32_t imm26 = (int32_t)(insn & 0x03ffffffu);
      if (imm26 & 0x02000000) imm26 |= (int32_t)0xfc000000; // Sign-extend 26-bit
      uintptr_t target = orig_pc + ((int64_t)imm26 * 4);
      int64_t new_diff = (int64_t)target - (int64_t)tramp_pc;
      if (new_diff < -0x8000000 || new_diff > 0x7fffffc || (new_diff & 3) != 0) {
        debugPrintf("[HOOK REJECT] Symbol at %p (+%d) B/BL out of range (diff=%lld)\n", (void *)addr, i * 4, (long long)new_diff);
        return 0;
      }
      int32_t new_imm26 = (int32_t)(new_diff >> 2) & 0x03ffffff;
      t_rw[i] = (insn & 0xfc000000u) | (uint32_t)new_imm26;
      debugPrintf("[TRAMP_RELOC] Relocated %s at %p (+%d) [0x%08x -> 0x%08x, target %p]\n",
                  (insn & 0x80000000u) ? "BL" : "B", (void *)addr, i * 4, insn, t_rw[i], (void *)target);
    }
    // LDR literal: (insn & 0x3b000000u) == 0x18000000u
    else if ((insn & 0x3b000000u) == 0x18000000u) {
      debugPrintf("[HOOK REJECT] Symbol at %p (+%d) uses LDR (literal) (0x%08x)\n", (void *)addr, i * 4, insn);
      return 0;
    }
    // CBZ / CBNZ: (insn & 0x7e000000u) == 0x34000000u
    else if ((insn & 0x7e000000u) == 0x34000000u) {
      debugPrintf("[HOOK REJECT] Symbol at %p (+%d) uses CBZ/CBNZ (0x%08x)\n", (void *)addr, i * 4, insn);
      return 0;
    }
    // TBZ / TBNZ: (insn & 0x7e000000u) == 0x36000000u
    else if ((insn & 0x7e000000u) == 0x36000000u) {
      debugPrintf("[HOOK REJECT] Symbol at %p (+%d) uses TBZ/TBNZ (0x%08x)\n", (void *)addr, i * 4, insn);
      return 0;
    }
    else {
      t_rw[i] = insn;
    }
  }

  t_rw[4] = 0x58000051u; // LDR X17, #0x8
  t_rw[5] = 0xd61f0220u; // BR X17
  *(uint64_t *)(t_rw + 6) = (uint64_t)(addr + 16);
  mod->tramp_used += TRAMPOLINE_STRIDE;

  const char *sym_name = so_nearest_symbol(mod, addr - (uintptr_t)mod->load_virtbase);
  debugPrintf("[TRAMPOLINE] Symbol '%s' at %p -> wrapper %p (tramp %p, resume %p, insns: 0x%08x 0x%08x 0x%08x 0x%08x)\n",
              sym_name ? sym_name : "?", (void *)addr, (void *)dst, (void *)t_virt, (void *)(addr + 16),
              t_rw[0], t_rw[1], t_rw[2], t_rw[3]);

  hook_arm64(addr, dst);

  return t_virt;
}
