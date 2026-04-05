#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[];  // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

struct {
  struct spinlock lock;
  struct shmem_region *regions;
  int next_anon_id;
} shmem_system;

// Make a direct-map page table for the kernel.
pagetable_t
kvmmake(void)
{
  pagetable_t kpgtbl;

  kpgtbl = (pagetable_t) kalloc();
  memset(kpgtbl, 0, PGSIZE);

  // uart registers
  kvmmap(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // PLIC
  kvmmap(kpgtbl, PLIC, PLIC, 0x4000000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

  // allocate and map a kernel stack for each process.
  proc_mapstacks(kpgtbl);
  
  return kpgtbl;
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void
kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm)
{
  if(mappages(kpgtbl, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

// Initialize the kernel_pagetable, shared by all CPUs.
void
kvminit(void)
{
  kernel_pagetable = kvmmake();
}

// Switch the current CPU's h/w page table register to
// the kernel's page table, and enable paging.
void
kvminithart()
{
  // wait for any previous writes to the page table memory to finish.
  sfence_vma();

  w_satp(MAKE_SATP(kernel_pagetable));

  // flush stale entries from the TLB.
  sfence_vma();
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  if(va >= MAXVA)
    panic("walk");

  for(int level = 2; level > 0; level--) {
    pte_t *pte = &pagetable[PX(level, va)];
    if(*pte & PTE_V) {
      pagetable = (pagetable_t)PTE2PA(*pte);
    } else {
      if(!alloc || (pagetable = (pde_t*)kalloc()) == 0)
        return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  return &pagetable[PX(0, va)];
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;

  if(va >= MAXVA)
    return 0;

  pte = walk(pagetable, va, 0);
  if(pte == 0)
    return 0;
  if((*pte & PTE_V) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  pa = PTE2PA(*pte);
  return pa;
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa.
// va and size MUST be page-aligned.
// Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("mappages: va not aligned");

  if((size % PGSIZE) != 0)
    panic("mappages: size not aligned");

  if(size == 0)
    panic("mappages: size");
  
  a = va;
  last = va + size - PGSIZE;
  for(;;){
    if((pte = walk(pagetable, a, 1)) == 0)
      return -1;
    if(*pte & PTE_V)
      panic("mappages: remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// create an empty user page table.
// returns 0 if out of memory.
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t) kalloc();
  if(pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. It's OK if the mappings don't exist.
// Optionally free the physical memory.
void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  for(a = va; a < va + npages*PGSIZE; a += PGSIZE){
    if((pte = walk(pagetable, a, 0)) == 0) // leaf page table entry allocated?
      continue;   
    if((*pte & PTE_V) == 0)  // has physical page been allocated?
      continue;
    uint64 pa = PTE2PA(*pte);

    if(do_free){
      // Special handling for shared memory
      // if(a == SHMEM_REGION){
      //   acquire(&shmem_page.lock);
      //   shmem_page.refcount--;
      //   // Only free the page when no one else uses it
      //   if(shmem_page.refcount == 0){
      //     kfree((void*)pa);
      //     shmem_page.allocated = 0;
      //   }
      //   release(&shmem_page.lock);
      // } else {
        // Normal pages
        kfree((void*)pa);
      // }
    }

    *pte = 0;
  }
}

// Allocate PTEs and physical memory to grow a process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int xperm)
{
  char *mem;
  uint64 a;

  if(newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for(a = oldsz; a < newsz; a += PGSIZE){
    mem = kalloc();
    if(mem == 0){
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if(mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_R|PTE_U|xperm) != 0){
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if(newsz >= oldsz)
    return oldsz;

  if(PGROUNDUP(newsz) < PGROUNDUP(oldsz)){
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
void
freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0){
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if(pte & PTE_V){
      panic("freewalk: leaf");
    }
  }
  kfree((void*)pagetable);
}

// Free user memory pages,
// then free page-table pages.
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  if(sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz)/PGSIZE, 1);
  freewalk(pagetable);
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;

  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walk(old, i, 0)) == 0)
      continue;   // page table entry hasn't been allocated
    if((*pte & PTE_V) == 0)
      continue;   // physical page hasn't been allocated
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    if((mem = kalloc()) == 0)
      goto err;
    memmove(mem, (char*)pa, PGSIZE);
    if(mappages(new, i, PGSIZE, (uint64)mem, flags) != 0){
      kfree(mem);
      goto err;
    }
  }
  //   // Handle shared memory region — lives above sz so the loop never reaches it
  // pte_t *shmem_pte = walk(old, SHMEM_REGION, 0);
  // if(shmem_pte && (*shmem_pte & PTE_V)){
  //   pa    = PTE2PA(*shmem_pte);
  //   flags = PTE_FLAGS(*shmem_pte);
  //   if(mappages(new, SHMEM_REGION, PGSIZE, pa, flags) != 0)
  //     goto err;
  //   acquire(&shmem_page.lock);
  //   shmem_page.refcount++;
  //   release(&shmem_page.lock);
  // }
  return 0;

 err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void
uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  
  pte = walk(pagetable, va, 0);
  if(pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;
  pte_t *pte;

  while(len > 0){
    va0 = PGROUNDDOWN(dstva);
    if(va0 >= MAXVA)
      return -1;
  
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0) {
      if((pa0 = vmfault(pagetable, va0, 0)) == 0) {
        return -1;
      }
    }

    pte = walk(pagetable, va0, 0);
    // forbid copyout over read-only user text pages.
    if((*pte & PTE_W) == 0)
      return -1;
      
    n = PGSIZE - (dstva - va0);
    if(n > len)
      n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int
copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  uint64 n, va0, pa0;

  while(len > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0) {
      if((pa0 = vmfault(pagetable, va0, 0)) == 0) {
        return -1;
      }
    }
    n = PGSIZE - (srcva - va0);
    if(n > len)
      n = len;
    memmove(dst, (void *)(pa0 + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }
  return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int
copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  uint64 n, va0, pa0;
  int got_null = 0;

  while(got_null == 0 && max > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if(n > max)
      n = max;

    char *p = (char *) (pa0 + (srcva - va0));
    while(n > 0){
      if(*p == '\0'){
        *dst = '\0';
        got_null = 1;
        break;
      } else {
        *dst = *p;
      }
      --n;
      --max;
      p++;
      dst++;
    }

    srcva = va0 + PGSIZE;
  }
  if(got_null){
    return 0;
  } else {
    return -1;
  }
}

// allocate and map user memory if process is referencing a page
// that was lazily allocated in sys_sbrk().
// returns 0 if va is invalid or already mapped, or if
// out of physical memory, and physical address if successful.
uint64
vmfault(pagetable_t pagetable, uint64 va, int read)
{
  uint64 mem;
  struct proc *p = myproc();

  if (va >= p->sz)
    return 0;
  va = PGROUNDDOWN(va);
  if(ismapped(pagetable, va)) {
    return 0;
  }
  mem = (uint64) kalloc();
  if(mem == 0)
    return 0;
  memset((void *) mem, 0, PGSIZE);
  if (mappages(p->pagetable, va, PGSIZE, mem, PTE_W|PTE_U|PTE_R) != 0) {
    kfree((void *)mem);
    return 0;
  }
  return mem;
}

int
ismapped(pagetable_t pagetable, uint64 va)
{
  pte_t *pte = walk(pagetable, va, 0);
  if (pte == 0) {
    return 0;
  }
  if (*pte & PTE_V){
    return 1;
  }
  return 0;
}

void
init_shmem(void)
{
  initlock(&shmem_system.lock, "shmem");
  shmem_system.regions = 0;
  shmem_system.next_anon_id = 1;
}

static int
shmem_perm(int prot)
{
  int perm = PTE_U;

  if(prot & PROT_READ)
    perm |= PTE_R;
  if(prot & PROT_WRITE)
    perm |= PTE_R | PTE_W;
  if(prot & PROT_EXEC)
    perm |= PTE_R | PTE_X;

  return perm;
}

static struct shmem_region *
shmem_find_region_locked(int id)
{
  struct shmem_region *r;

  for(r = shmem_system.regions; r; r = r->next){
    if(r->id == id)
      return r;
  }

  return 0;
}

static void
shmem_unlink_region_locked(struct shmem_region *region)
{
  struct shmem_region *r, *prev = 0;

  for(r = shmem_system.regions; r; r = r->next){
    if(r != region){
      prev = r;
      continue;
    }

    if(prev)
      prev->next = r->next;
    else
      shmem_system.regions = r->next;
    return;
  }
}

static void
shmem_region_free_locked(struct shmem_region *region)
{
  for(int i = 0; i < region->npages; i++){
    if(region->pages[i])
      kfree((void*)region->pages[i]);
  }
  kfree((void*)region);
}

static void
shmem_region_put_locked(struct shmem_region *region)
{
  if(region->refcount > 0)
    region->refcount--;

  if(region->refcount == 0){
    shmem_unlink_region_locked(region);
    shmem_region_free_locked(region);
  }
}

static struct shmem_region *
shmem_region_create_locked(uint64 len, int id)
{
  struct shmem_region *region;

  if(len == 0 || len % PGSIZE != 0)
    return 0;

  region = (struct shmem_region*)kalloc();
  if(region == 0)
    return 0;
  memset(region, 0, PGSIZE);

  region->id = id;
  region->size = len;
  region->npages = len / PGSIZE;
  if(region->npages > MAX_SHMEM_REGION_PAGES){
    kfree((void*)region);
    return 0;
  }

  for(int i = 0; i < region->npages; i++){
    char *mem = kalloc();
    if(mem == 0){
      for(int j = 0; j < i; j++)
        kfree((void*)region->pages[j]);
      kfree((void*)region);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    region->pages[i] = (uint64)mem;
  }

  region->next = shmem_system.regions;
  shmem_system.regions = region;

  return region;
}

static struct proc_mmap *
shmem_mapping_alloc(void)
{
  struct proc_mmap *mapping;

  mapping = (struct proc_mmap*)kalloc();
  if(mapping)
    memset(mapping, 0, PGSIZE);
  return mapping;
}

static void
shmem_mapping_insert(struct proc *p, struct proc_mmap *mapping)
{
  mapping->next = p->mmaps;
  p->mmaps = mapping;
}

static int
shmem_range_free(pagetable_t pagetable, uint64 start, uint64 len)
{
  uint64 end = start + len;

  for(uint64 va = start; va < end; va += PGSIZE){
    if(ismapped(pagetable, va))
      return 0;
  }

  return 1;
}

static uint64
shmem_find_va(struct proc *p, uint64 len)
{
  uint64 floor = PGROUNDUP(p->sz);
  uint64 va;

  if(len == 0 || floor + len > TRAPFRAME)
    return 0;

  va = PGROUNDDOWN(TRAPFRAME - len);
  for(;;){
    if(va < floor)
      return 0;
    if(shmem_range_free(p->pagetable, va, len))
      return va;
    if(va < PGSIZE)
      return 0;
    va -= PGSIZE;
  }
}

static int
shmem_map_pages(pagetable_t pagetable, uint64 va, struct shmem_region *region, int prot)
{
  int perm = shmem_perm(prot);
  uint64 mapped = 0;

  if((perm & (PTE_R | PTE_W | PTE_X)) == 0)
    return -1;

  for(int i = 0; i < region->npages; i++){
    if(mappages(pagetable, va + mapped, PGSIZE, region->pages[i], perm) != 0){
      if(mapped > 0)
        uvmunmap(pagetable, va, mapped / PGSIZE, 0);
      return -1;
    }
    mapped += PGSIZE;
  }

  return 0;
}

uint64
shmem_mmap_limit(struct proc *p)
{
  uint64 limit = TRAPFRAME;

  for(struct proc_mmap *m = p->mmaps; m; m = m->next){
    if(m->va < limit)
      limit = m->va;
  }

  return limit;
}

void
proc_freeshmem(struct proc *p, pagetable_t pagetable)
{
  struct proc_mmap *mapping = p->mmaps;

  p->mmaps = 0;
  while(mapping){
    struct proc_mmap *next = mapping->next;

    if(pagetable)
      uvmunmap(pagetable, mapping->va, mapping->len / PGSIZE, 0);

    acquire(&shmem_system.lock);
    shmem_region_put_locked(mapping->region);
    release(&shmem_system.lock);

    kfree((void*)mapping);
    mapping = next;
  }
}

int
shmem_copy_mappings(struct proc *parent, struct proc *child)
{
  for(struct proc_mmap *src = parent->mmaps; src; src = src->next){
    struct proc_mmap *dst = shmem_mapping_alloc();
    if(dst == 0)
      goto err;

    if(shmem_map_pages(child->pagetable, src->va, src->region, src->prot) < 0){
      kfree((void*)dst);
      goto err;
    }

    acquire(&shmem_system.lock);
    src->region->refcount++;
    release(&shmem_system.lock);

    dst->va = src->va;
    dst->len = src->len;
    dst->prot = src->prot;
    dst->flags = src->flags;
    dst->region = src->region;
    shmem_mapping_insert(child, dst);
  }

  return 0;

 err:
  proc_freeshmem(child, child->pagetable);
  return -1;
}

uint64
mmap(uint64 len, int prot, int flags, int id)
{
  struct proc *p = myproc();
  struct shmem_region *region;
  struct proc_mmap *mapping;
  uint64 va;

  len = PGROUNDUP(len);
  if(len == 0 || len / PGSIZE > MAX_SHMEM_REGION_PAGES)
    return 0;
  if((prot & (PROT_READ | PROT_WRITE | PROT_EXEC)) == 0)
    return 0;
  if((flags & MAP_SHARED) == 0)
    return 0;
  if(id < 0)
    return 0;

  mapping = shmem_mapping_alloc();
  if(mapping == 0)
    return 0;

  acquire(&shmem_system.lock);
  if(id == 0 || (flags & MAP_ANON)){
    region = shmem_region_create_locked(len, -shmem_system.next_anon_id++);
  } else {
    region = shmem_find_region_locked(id);
    if(region && region->size != len){
      release(&shmem_system.lock);
      kfree((void*)mapping);
      return 0;
    }
    if(region == 0)
      region = shmem_region_create_locked(len, id);
  }

  if(region == 0){
    release(&shmem_system.lock);
    kfree((void*)mapping);
    return 0;
  }

  region->refcount++;
  release(&shmem_system.lock);

  va = shmem_find_va(p, len);
  if(va == 0 || shmem_map_pages(p->pagetable, va, region, prot) < 0){
    acquire(&shmem_system.lock);
    shmem_region_put_locked(region);
    release(&shmem_system.lock);
    kfree((void*)mapping);
    return 0;
  }

  mapping->va = va;
  mapping->len = len;
  mapping->prot = prot;
  mapping->flags = flags;
  mapping->region = region;
  shmem_mapping_insert(p, mapping);

  return va;
}

int
munmap(uint64 va)
{
  struct proc *p = myproc();
  struct proc_mmap *mapping = p->mmaps;
  struct proc_mmap *prev = 0;

  while(mapping && mapping->va != va){
    prev = mapping;
    mapping = mapping->next;
  }

  if(mapping == 0)
    return -1;

  uvmunmap(p->pagetable, mapping->va, mapping->len / PGSIZE, 0);

  if(prev)
    prev->next = mapping->next;
  else
    p->mmaps = mapping->next;

  acquire(&shmem_system.lock);
  shmem_region_put_locked(mapping->region);
  release(&shmem_system.lock);

  kfree((void*)mapping);
  return 0;
}
