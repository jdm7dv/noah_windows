#include "common.h"

#include "noah.h"
#include "vm.h"
#include "mm.h"
#include "x86/vm.h"

#include "linux/mman.h"

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#include <sys/mman.h>
#include <pthread.h>
#ifdef __APPLE__
#include <Hypervisor/hv.h>
#endif
#endif


void
init_mmap(struct mm *mm)
{
  mm->current_mmap_top = 0x00000000c0000000;
}

gaddr_t
alloc_region(size_t len)
{
  len = roundup(len, PAGE_SIZE(PAGE_4KB));
  proc->mm->current_mmap_top += len;
  return proc->mm->current_mmap_top - len;
}

int
do_munmap(gaddr_t gaddr, size_t size)
{
  if (!is_page_aligned((void*)gaddr, PAGE_4KB)) {
    return -LINUX_EINVAL;
  }
  size = roundup(size, PAGE_SIZE(PAGE_4KB)); // Linux kernel also does this

  struct mm_region *overlapping = find_region_range(gaddr, size, proc->mm);
  if (overlapping == NULL) {
    return -LINUX_ENOMEM;
  }

  struct mm_region key = {.gaddr = gaddr, .size = size};
  while (region_compare(&key, overlapping) == 0) {
    if (overlapping->gaddr < gaddr) {
      split_region(proc->mm, overlapping, gaddr);
      overlapping = list_entry(overlapping->list.next, struct mm_region, list);
    }
    if (overlapping->gaddr + overlapping->size > gaddr + size) {
      split_region(proc->mm, overlapping, gaddr + size);
    }
    struct list_head *next = overlapping->list.next;
    list_del(&overlapping->list);
    RB_REMOVE(mm_region_tree, &proc->mm->mm_region_tree, overlapping);
    vm_munmap(overlapping->gaddr, overlapping->size);
    platform_unmap_mem(overlapping->haddr, overlapping->handle, overlapping->size);
    free(overlapping);
    if (next == &proc->mm->mm_regions)
      break;
    overlapping = list_entry(next, struct mm_region, list);
  }

  return 0;
}

gaddr_t
do_mmap(gaddr_t addr, size_t len, int n_prot, int l_prot, int l_flags, int fd, off_t offset)
{
  assert((addr & 0xfff) == 0);
  if (!(l_flags & LINUX_MAP_PRIVATE) && !(l_flags & LINUX_MAP_ANON)) {
    return -LINUX_EINVAL;
  }

  /* some l_flags are obsolete and just ignored */
  l_flags &= ~LINUX_MAP_DENYWRITE;
  l_flags &= ~LINUX_MAP_EXECUTABLE;

  /* We ignore these currenlty */
  l_flags &= ~LINUX_MAP_NORESERVE;

  /* the linux kernel does nothing for LINUX_MAP_STACK */
  l_flags &= ~LINUX_MAP_STACK;

  len = roundup(len, PAGE_SIZE(PAGE_4KB));

  if ((l_flags & ~(LINUX_MAP_SHARED | LINUX_MAP_PRIVATE | LINUX_MAP_FIXED | LINUX_MAP_ANON)) != 0) {
    warnk("unsupported mmap l_flags: 0x%x\n", l_flags);
    exit(1);
  }
  if (l_flags & LINUX_MAP_ANON) {
    fd = -1;
    offset = 0;
  }
  if ((l_flags & LINUX_MAP_FIXED) == 0) {
    addr = alloc_region(len);
  }

  void *ptr;
  platform_handle_t handle;
  int err;
  if (!(l_flags & LINUX_MAP_ANON)) {
    // TODO
    return -LINUX_EINVAL;
  } else {
    err = platform_map_mem(&ptr, &handle, len, n_prot, linux_to_native_mflags(l_flags));
  }
  if (err < 0) {
    panic("mmap failed. addr :0x%llx, len: 0x%lux, prot: %d, l_flags: %d, fd: %d, offset: 0x%llx\n", addr, len, l_prot, l_flags, fd, offset);
  }

  do_munmap(addr, len);
  record_region(proc->mm, handle, ptr, addr, len, l_prot, l_flags, fd, offset);

  vm_mmap(addr, len, linux_to_native_mprot(l_prot), ptr);

  return addr;
}
