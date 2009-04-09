/*
   Copyright (C) 1994-2001 Digitool, Inc
   This file is part of OpenMCL.  

   OpenMCL is licensed under the terms of the Lisp Lesser GNU Public
   License , known as the LLGPL and distributed with OpenMCL as the
   file "LICENSE".  The LLGPL consists of a preamble and the LGPL,
   which is distributed with OpenMCL as the file "LGPL".  Where these
   conflict, the preamble takes precedence.  

   OpenMCL is referenced in the preamble as the "LIBRARY."

   The LLGPL is also available online at
   http://opensource.franz.com/preamble.html
*/

#ifdef DARWIN
/*	dyld.h included here because something in "lisp.h" causes
    a conflict (actually I think the problem is in "constants.h")
*/
#include <mach-o/dyld.h>

#endif
#include "lisp.h"
#include "lisp_globals.h"
#include "gc.h"
#include "area.h"
#include <stdlib.h>
#include <string.h>
#include "lisp-exceptions.h"
#include <stdio.h>
#include <stdlib.h>
#ifndef WINDOWS
#include <sys/mman.h>
#endif
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#ifndef WINDOWS
#include <sys/utsname.h>
#include <unistd.h>
#endif

#ifdef LINUX
#include <mcheck.h>
#include <dirent.h>
#include <dlfcn.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <link.h>
#include <elf.h>

/* 
   The version of <asm/cputable.h> provided by some distributions will
   claim that <asm-ppc64/cputable.h> doesn't exist.  It may be present
   in the Linux kernel source tree even if it's not copied to
   /usr/include/asm-ppc64.  Hopefully, this will be straightened out
   soon (and/or the PPC_FEATURE_HAS_ALTIVEC constant will be defined
   in a less volatile place.)  Until that's straightened out, it may
   be necessary to install a copy of the kernel header in the right
   place and/or persuade <asm/cputable> to lighten up a bit.
*/

#ifdef PPC
#ifndef PPC64
#include <asm/cputable.h>
#endif
#ifndef PPC_FEATURE_HAS_ALTIVEC
#define PPC_FEATURE_HAS_ALTIVEC 0x10000000
#endif
#endif
#endif

Boolean use_mach_exception_handling = 
#ifdef DARWIN
  true
#else
  false
#endif
;

#ifdef DARWIN
#include <sys/types.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <mach/mach_types.h>
#include <mach/message.h>
#include <mach/vm_region.h>
#include <mach/port.h>
#include <sys/sysctl.h>

Boolean running_under_rosetta = false;

#if WORD_SIZE == 64 || defined(X8632)
/* Assume that if the OS is new enough to support PPC64/X8664, it has
   a reasonable dlfcn.h
*/
#include <dlfcn.h>
#endif
#endif

#if defined(FREEBSD) || defined(SOLARIS)
#include <sys/time.h>
#include <sys/resource.h>
#include <dlfcn.h>
#include <elf.h> 
#include <link.h>
#endif

#include <ctype.h>
#ifndef WINDOWS
#include <sys/select.h>
#endif
#include "Threads.h"

#include <fenv.h>
#include <sys/stat.h>

#ifndef MAP_NORESERVE
#define MAP_NORESERVE (0)
#endif

#ifdef WINDOWS
#include <windows.h>
#include <stdio.h>
void
wperror(char* message)
{
  char* buffer;
  DWORD last_error = GetLastError();
  
  FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER|
		FORMAT_MESSAGE_FROM_SYSTEM|
		FORMAT_MESSAGE_IGNORE_INSERTS,
		NULL,
		last_error,
		MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		(LPTSTR)&buffer,
		0, NULL);
  fprintf(dbgout, "%s: 0x%x %s\n", message, (unsigned) last_error, buffer);
  LocalFree(buffer);
}
#endif

LispObj lisp_nil = (LispObj) 0;
bitvector global_mark_ref_bits = NULL;


/* These are all "persistent" : they're initialized when
   subprims are first loaded and should never change. */
extern LispObj ret1valn;
extern LispObj nvalret;
extern LispObj popj;
#ifdef X86
extern LispObj bad_funcall;
#endif

LispObj text_start = 0;

/* A pointer to some of the kernel's own data; also persistent. */

extern LispObj import_ptrs_base;



void
xMakeDataExecutable(void *, unsigned long);

void
make_dynamic_heap_executable(LispObj *p, LispObj *q)
{
  void * cache_start = (void *) p;
  natural ncacheflush = (natural) q - (natural) p;

  xMakeDataExecutable(cache_start, ncacheflush);  
}
      
size_t
ensure_stack_limit(size_t stack_size)
{
#ifdef WINDOWS
  extern void os_get_current_thread_stack_bounds(void **, natural*);
  natural totalsize;
  void *ignored;
  
  os_get_current_thread_stack_bounds(&ignored, &totalsize);

  return (size_t)totalsize-(size_t)(CSTACK_HARDPROT+CSTACK_SOFTPROT);

#else
  struct rlimit limits;
  rlim_t cur_stack_limit, max_stack_limit;
 
  stack_size += (CSTACK_HARDPROT+CSTACK_SOFTPROT);
  getrlimit(RLIMIT_STACK, &limits);
  cur_stack_limit = limits.rlim_cur;
  max_stack_limit = limits.rlim_max;
  if (stack_size > max_stack_limit) {
    stack_size = max_stack_limit;
  }
  if (cur_stack_limit < stack_size) {
    limits.rlim_cur = stack_size;
    errno = 0;
    if (setrlimit(RLIMIT_STACK, &limits)) {
      int e = errno;
      fprintf(dbgout, "errno = %d\n", e);
      Fatal(": Stack resource limit too small", "");
    }
  }
#endif
  return stack_size;
}


/* This should write-protect the bottom of the stack.
   Doing so reliably involves ensuring that everything's unprotected on exit.
*/

BytePtr
allocate_lisp_stack(natural useable,
                    unsigned softsize,
                    unsigned hardsize,
                    lisp_protection_kind softkind,
                    lisp_protection_kind hardkind,
                    Ptr *h_p,
                    BytePtr *base_p,
                    protected_area_ptr *softp,
                    protected_area_ptr *hardp)
{
  void *allocate_stack(natural);
  void free_stack(void *);
  natural size = useable+softsize+hardsize;
  natural overhead;
  BytePtr base, softlimit, hardlimit;
  Ptr h = allocate_stack(size+4095);
  protected_area_ptr hprotp = NULL, sprotp;

  if (h == NULL) {
    return NULL;
  }
  if (h_p) *h_p = h;
  base = (BytePtr) align_to_power_of_2( h, log2_page_size);
  hardlimit = (BytePtr) (base+hardsize);
  softlimit = hardlimit+softsize;

  overhead = (base - (BytePtr) h);
  if (hardsize) {
    hprotp = new_protected_area((BytePtr)base,hardlimit,hardkind, hardsize, true);
    if (hprotp == NULL) {
      if (base_p) *base_p = NULL;
      if (h_p) *h_p = NULL;
      deallocate(h);
      return NULL;
    }
    if (hardp) *hardp = hprotp;
  }
  if (softsize) {
    sprotp = new_protected_area(hardlimit,softlimit, softkind, softsize, true);
    if (sprotp == NULL) {
      if (base_p) *base_p = NULL;
      if (h_p) *h_p = NULL;
      if (hardp) *hardp = NULL;
      if (hprotp) delete_protected_area(hprotp);
      free_stack(h);
      return NULL;
    }
    if (softp) *softp = sprotp;
  }
  if (base_p) *base_p = base;
  return (BytePtr) ((natural)(base+size));
}

/*
  This should only called by something that owns the area_lock, or
  by the initial thread before other threads exist.
*/
area *
allocate_lisp_stack_area(area_code stack_type,
                         natural usable,
                         unsigned softsize, 
                         unsigned hardsize, 
                         lisp_protection_kind softkind, 
                         lisp_protection_kind hardkind)

{
  BytePtr base, bottom;
  Ptr h;
  area *a = NULL;
  protected_area_ptr soft_area=NULL, hard_area=NULL;

  bottom = allocate_lisp_stack(usable, 
                               softsize, 
                               hardsize, 
                               softkind, 
                               hardkind, 
                               &h, 
                               &base,
                               &soft_area, 
                               &hard_area);

  if (bottom) {
    a = new_area(base, bottom, stack_type);
    a->hardlimit = base+hardsize;
    a->softlimit = base+hardsize+softsize;
    a->h = h;
    a->softprot = soft_area;
    a->hardprot = hard_area;
    add_area_holding_area_lock(a);
  }
  return a;
}

/*
  Also assumes ownership of the area_lock 
*/
area*
register_cstack_holding_area_lock(BytePtr bottom, natural size)
{
  BytePtr lowlimit = (BytePtr) (((((natural)bottom)-size)+4095)&~4095);
  area *a = new_area((BytePtr) bottom-size, bottom, AREA_CSTACK);
  a->hardlimit = lowlimit+CSTACK_HARDPROT;
  a->softlimit = a->hardlimit+CSTACK_SOFTPROT;
#ifdef USE_SIGALTSTACK
  setup_sigaltstack(a);
#endif
  add_area_holding_area_lock(a);
  return a;
}
  

area*
allocate_vstack_holding_area_lock(natural usable)
{
  return allocate_lisp_stack_area(AREA_VSTACK, 
				  usable > MIN_VSTACK_SIZE ?
				  usable : MIN_VSTACK_SIZE,
                                  VSTACK_SOFTPROT,
                                  VSTACK_HARDPROT,
                                  kVSPsoftguard,
                                  kVSPhardguard);
}

area *
allocate_tstack_holding_area_lock(natural usable)
{
  return allocate_lisp_stack_area(AREA_TSTACK, 
                                  usable > MIN_TSTACK_SIZE ?
				  usable : MIN_TSTACK_SIZE,
                                  TSTACK_SOFTPROT,
                                  TSTACK_HARDPROT,
                                  kTSPsoftguard,
                                  kTSPhardguard);
}


/* It's hard to believe that max & min don't exist already */
unsigned unsigned_min(unsigned x, unsigned y)
{
  if (x <= y) {
    return x;
  } else {
    return y;
  }
}

unsigned unsigned_max(unsigned x, unsigned y)
{
  if (x >= y) {
    return x;
  } else {
    return y;
  }
}

#if WORD_SIZE == 64
#ifdef DARWIN
#define MAXIMUM_MAPPABLE_MEMORY (512L<<30L)
#endif
#ifdef FREEBSD
#define MAXIMUM_MAPPABLE_MEMORY (512L<<30L)
#endif
#ifdef SOLARIS
#define MAXIMUM_MAPPABLE_MEMORY (128L<<30L)
#endif
#ifdef LINUX
#ifdef X8664
#define MAXIMUM_MAPPABLE_MEMORY (512L<<30L)
#endif
#ifdef PPC
#define MAXIMUM_MAPPABLE_MEMORY (128L<<30L)
#endif
#endif
#ifdef WINDOWS
/* Supposedly, the high-end version of Vista allow 128GB of pageable memory */
#define MAXIMUM_MAPPABLE_MEMORY (120LL<<30LL)
#endif
#else
#ifdef DARWIN
#define MAXIMUM_MAPPABLE_MEMORY ((1U<<31)-2*heap_segment_size)
#endif
#ifdef LINUX
#ifdef X86
#define MAXIMUM_MAPPABLE_MEMORY (9U<<28)
#else
#define MAXIMUM_MAPPABLE_MEMORY (1U<<30)
#endif
#endif
#ifdef WINDOWS
#define MAXIMUM_MAPPABLE_MEMORY (1U<<30)
#endif
#ifdef FREEBSD
#define MAXIMUM_MAPPABLE_MEMORY (1U<<30)
#endif
#ifdef SOLARIS
#define MAXIMUM_MAPPABLE_MEMORY (1U<<30)
#endif
#endif

natural
reserved_area_size = MAXIMUM_MAPPABLE_MEMORY;

area 
  *nilreg_area=NULL,
  *tenured_area=NULL, 
  *g2_area=NULL, 
  *g1_area=NULL,
  *managed_static_area=NULL,
  *readonly_area=NULL;

area *all_areas=NULL;
int cache_block_size=32;


#if WORD_SIZE == 64
#define DEFAULT_LISP_HEAP_GC_THRESHOLD (32<<20)
#define G2_AREA_THRESHOLD (8<<20)
#define G1_AREA_THRESHOLD (4<<20)
#define G0_AREA_THRESHOLD (2<<20)
#else
#define DEFAULT_LISP_HEAP_GC_THRESHOLD (16<<20)
#define G2_AREA_THRESHOLD (4<<20)
#define G1_AREA_THRESHOLD (2<<20)
#define G0_AREA_THRESHOLD (1<<20)
#endif

#define MIN_DYNAMIC_SIZE (DEFAULT_LISP_HEAP_GC_THRESHOLD *2)

#if (WORD_SIZE == 32)
#define DEFAULT_INITIAL_STACK_SIZE (1<<20)
#else
#define DEFAULT_INITIAL_STACK_SIZE (2<<20)
#endif

natural
lisp_heap_gc_threshold = DEFAULT_LISP_HEAP_GC_THRESHOLD;

natural 
initial_stack_size = DEFAULT_INITIAL_STACK_SIZE;

natural
thread_stack_size = 0;


/*
  'start' should be on a segment boundary; 'len' should be
  an integral number of segments.  remap the entire range.
*/

void 
uncommit_pages(void *start, size_t len)
{
  UnCommitMemory(start, len);
}

#define TOUCH_PAGES_ON_COMMIT 0

Boolean
touch_all_pages(void *start, size_t len)
{
#if TOUCH_PAGES_ON_COMMIT
  extern Boolean touch_page(void *);
  char *p = (char *)start;

  while (len) {
    if (!touch_page(p)) {
      return false;
    }
    len -= page_size;
    p += page_size;
  }
#endif
  return true;
}

Boolean
commit_pages(void *start, size_t len)
{
  if (len != 0) {
    if (CommitMemory(start, len)) {
      if (touch_all_pages(start, len)) {
	return true;
      }
    }
  }
  return true;
}

area *
find_readonly_area()
{
  area *a;

  for (a = active_dynamic_area->succ; a != all_areas; a = a->succ) {
    if (a->code == AREA_READONLY) {
      return a;
    }
  }
  return NULL;
}

area *
extend_readonly_area(unsigned more)
{
  area *a;
  unsigned mask;
  BytePtr new_start, new_end;

  if ((a = find_readonly_area()) != NULL) {
    if ((a->active + more) > a->high) {
      return NULL;
    }
    mask = ((natural)a->active) & (page_size-1);
    if (mask) {
      UnProtectMemory(a->active-mask, page_size);
    }
    new_start = (BytePtr)(align_to_power_of_2(a->active,log2_page_size));
    new_end = (BytePtr)(align_to_power_of_2(a->active+more,log2_page_size));
    if (!CommitMemory(new_start, new_end-new_start)) {
      return NULL;
    }
    return a;
  }
  return NULL;
}

LispObj image_base=0;
BytePtr pure_space_start, pure_space_active, pure_space_limit;
BytePtr static_space_start, static_space_active, static_space_limit;

void
raise_limit()
{
#ifdef RLIMIT_AS
  struct rlimit r;
  if (getrlimit(RLIMIT_AS, &r) == 0) {
    r.rlim_cur = r.rlim_max;
    setrlimit(RLIMIT_AS, &r);
    /* Could limit heaplimit to rlim_max here if smaller? */
  }
#endif
} 


area *
create_reserved_area(natural totalsize)
{
  Ptr h;
  natural base;
  BytePtr 
    end, 
    lastbyte, 
    start, 
    want = (BytePtr)IMAGE_BASE_ADDRESS;
  area *reserved;
  Boolean fatal = false;

  totalsize = align_to_power_of_2((void *)totalsize, log2_heap_segment_size);
    
  if (totalsize < (PURESPACE_RESERVE + MIN_DYNAMIC_SIZE)) {
    totalsize = PURESPACE_RESERVE + MIN_DYNAMIC_SIZE;
    fatal = true;
  }

  start = ReserveMemoryForHeap(want, totalsize);

  if (start == NULL) {
    if (fatal) {
      perror("minimal initial mmap");
      exit(1);
    }
    return NULL;
  }

  h = (Ptr) start;
  base = (natural) start;
  image_base = base;
  lastbyte = (BytePtr) (start+totalsize);
  static_space_start = static_space_active = (BytePtr)STATIC_BASE_ADDRESS;
  static_space_limit = static_space_start + STATIC_RESERVE;
  pure_space_start = pure_space_active = start;
  pure_space_limit = start + PURESPACE_RESERVE;
  start = pure_space_limit;

  /*
    Allocate mark bits here.  They need to be 1/64 the size of the
     maximum useable area of the heap (+ 3 words for the EGC.)
  */
  end = lastbyte;
  end = (BytePtr) ((natural)((((natural)end) - ((totalsize+63)>>6)) & ~4095));

  global_mark_ref_bits = (bitvector)end;
  end = (BytePtr) ((natural)((((natural)end) - ((totalsize+63) >> 6)) & ~4095));
  global_reloctab = (LispObj *) end;
  reserved = new_area(start, end, AREA_VOID);
  /* The root of all evil is initially linked to itself. */
  reserved->pred = reserved->succ = reserved;
  all_areas = reserved;
  reserved->markbits = global_mark_ref_bits;
  return reserved;
}

void *
allocate_from_reserved_area(natural size)
{
  area *reserved = reserved_area;
  BytePtr low = reserved->low, high = reserved->high;
  natural avail = high-low;
  
  size = align_to_power_of_2(size, log2_heap_segment_size);

  if (size > avail) {
    return NULL;
  }
  reserved->low += size;
  reserved->active = reserved->low;
  reserved->ndnodes -= (size>>dnode_shift);
  return low;
}



BytePtr reloctab_limit = NULL, markbits_limit = NULL;

void
ensure_gc_structures_writable()
{
  natural 
    ndnodes = area_dnode(lisp_global(HEAP_END),lisp_global(HEAP_START)),
    markbits_size = (3*sizeof(LispObj))+((ndnodes+7)>>3),
    reloctab_size = (sizeof(LispObj)*(((ndnodes+((1<<bitmap_shift)-1))>>bitmap_shift)+1));
  BytePtr 
    new_reloctab_limit = ((BytePtr)global_reloctab)+reloctab_size,
    new_markbits_limit = ((BytePtr)global_mark_ref_bits)+markbits_size;

  if (new_reloctab_limit > reloctab_limit) {
    CommitMemory(global_reloctab, reloctab_size);
    UnProtectMemory(global_reloctab, reloctab_size);
    reloctab_limit = new_reloctab_limit;
  }
  
  if (new_markbits_limit > markbits_limit) {
    CommitMemory(global_mark_ref_bits, markbits_size);
    UnProtectMemory(global_mark_ref_bits, markbits_size);
    markbits_limit = new_markbits_limit;
  }
}


area *
allocate_dynamic_area(natural initsize)
{
  natural totalsize = align_to_power_of_2(initsize, log2_heap_segment_size);
  BytePtr start, end;
  area *a;

  start = allocate_from_reserved_area(totalsize);
  if (start == NULL) {
    fprintf(dbgout, "reserved area too small to load heap image\n");
    exit(1);
  }
  end = start + totalsize;
  a = new_area(start, end, AREA_DYNAMIC);
  a->active = start+initsize;
  add_area_holding_area_lock(a);
  a->markbits = reserved_area->markbits;
  reserved_area->markbits = NULL;
  CommitMemory(start, end-start);
  a->h = start;
  a->softprot = NULL;
  a->hardprot = NULL;
  lisp_global(HEAP_START) = ptr_to_lispobj(a->low);
  lisp_global(HEAP_END) = ptr_to_lispobj(a->high);
  ensure_gc_structures_writable();
  return a;
 }


Boolean
grow_dynamic_area(natural delta)
{
  area *a = active_dynamic_area, *reserved = reserved_area;
  natural avail = reserved->high - reserved->low;
  
  delta = align_to_power_of_2(delta, log2_heap_segment_size);
  if (delta > avail) {
    return false;
  }

  if (!commit_pages(a->high,delta)) {
    return false;
  }


  if (!allocate_from_reserved_area(delta)) {
    return false;
  }


  a->high += delta;
  a->ndnodes = area_dnode(a->high, a->low);
  lisp_global(HEAP_END) += delta;
  ensure_gc_structures_writable();
  return true;
}

/*
  As above.  Pages that're returned to the reserved_area are
  "condemned" (e.g, we try to convince the OS that they never
  existed ...)
*/
Boolean
shrink_dynamic_area(natural delta)
{
  area *a = active_dynamic_area, *reserved = reserved_area;
  
  delta = align_to_power_of_2(delta, log2_heap_segment_size);

  a->high -= delta;
  a->ndnodes = area_dnode(a->high, a->low);
  a->hardlimit = a->high;
  uncommit_pages(a->high, delta);
  reserved->low -= delta;
  reserved->ndnodes += (delta>>dnode_shift);
  lisp_global(HEAP_END) -= delta;
  return true;
}



void
user_signal_handler (int signum, siginfo_t *info, ExceptionInformation *context)
{
  if (signum == SIGINT) {
    lisp_global(INTFLAG) = (1 << fixnumshift);
  }
  else if (signum == SIGTERM) {
    lisp_global(INTFLAG) = (2 << fixnumshift);
  }
#ifdef DARWIN
  DarwinSigReturn(context);
#endif
}


void
register_user_signal_handler()
{
#ifdef WINDOWS
  extern BOOL CALLBACK ControlEventHandler(DWORD);

  signal(SIGINT, SIG_IGN);

  SetConsoleCtrlHandler(ControlEventHandler,TRUE);
#else
  install_signal_handler(SIGINT, (void *)user_signal_handler);
  install_signal_handler(SIGTERM, (void *)user_signal_handler);
#endif
}



BytePtr
initial_stack_bottom()
{
#ifndef WINDOWS
  extern char **environ;
  char *p = *environ;
  while (*p) {
    p += (1+strlen(p));
  }
  return (BytePtr)((((natural) p) +4095) & ~4095);
#endif
#ifdef WINDOWS
  return (BytePtr)((current_stack_pointer() + 4095) & ~ 4095);
#endif
}


  
Ptr fatal_spare_ptr = NULL;


void
Fatal(StringPtr param0, StringPtr param1)
{

  if (fatal_spare_ptr) {
    deallocate(fatal_spare_ptr);
    fatal_spare_ptr = NULL;
  }
  fprintf(dbgout, "Fatal error: %s\n%s\n", param0, param1);
  _exit(-1);
}

OSErr application_load_err = noErr;

area *
set_nil(LispObj);


/* Check for the existence of a file named by 'path'; return true
   if it seems to exist, without checking size, permissions, or
   anything else. */
Boolean
probe_file(char *path)
{
  struct stat st;

  return (stat(path,&st) == 0);
}


#ifdef WINDOWS
/* Chop the trailing ".exe" from the kernel image name */
char *
chop_exe_suffix(char *path)
{
  int len = strlen(path);
  char *copy = malloc(len+1), *tail;

  strcpy(copy,path);
  tail = strrchr(copy, '.');
  if (tail) {
    *tail = 0;
  }
  return copy;
}
#endif

char *
path_by_appending_image(char *path)
{
  int len = strlen(path) + strlen(".image") + 1;
  char *copy = (char *) malloc(len);

  if (copy) {
    strcpy(copy, path);
    strcat(copy, ".image");
  }
  return copy;
}

char *
case_inverted_path(char *path)
{
  char *copy = strdup(path), *base = copy, *work = copy, c;
  if (copy == NULL) {
    return NULL;
  }
  while(*work) {
    if (*work++ == '/') {
      base = work;
    }
  }
  work = base;
  while ((c = *work) != '\0') {
    if (islower(c)) {
      *work++ = toupper(c);
    } else {
      *work++ = tolower(c);
    }
  }
  return copy;
}
/* 
   The underlying file system may be case-insensitive (e.g., HFS),
   so we can't just case-invert the kernel's name.
   Tack ".image" onto the end of the kernel's name.  Much better ...
*/
char *
default_image_name(char *orig)
{
#ifdef WINDOWS
  char *path = chop_exe_suffix(orig);
#else
  char *path = orig;
#endif
  char *image_name = path_by_appending_image(path);
#if !defined(WINDOWS) && !defined(DARWIN)
  if (!probe_file(image_name)) {
    char *legacy = case_inverted_path(path);
    if (probe_file(legacy)) {
      image_name = legacy;
    }
  }
#endif
  return image_name;
}



char *program_name = NULL;
char *real_executable_name = NULL;

char *
determine_executable_name(char *argv0)
{
#ifdef DARWIN
  uint32_t len = 1024;
  char exepath[1024], *p = NULL;

  if (_NSGetExecutablePath(exepath, (void *)&len) == 0) {
    p = malloc(len+1);
    memmove(p, exepath, len);
    p[len]=0;
    return p;
  } 
  return argv0;
#endif
#ifdef LINUX
  char exepath[PATH_MAX], *p;
  int n;

  if ((n = readlink("/proc/self/exe", exepath, PATH_MAX)) > 0) {
    p = malloc(n+1);
    memmove(p,exepath,n);
    p[n]=0;
    return p;
  }
  return argv0;
#endif
#ifdef FREEBSD
  return argv0;
#endif
#ifdef SOLARIS
  char exepath[PATH_MAX], proc_path[PATH_MAX], *p;
  int n;

  snprintf(proc_path,PATH_MAX-1,"/proc/%d/path/a.out",getpid());

  if ((n = readlink(proc_path, exepath, PATH_MAX)) > 0) {
    p = malloc(n+1);
    memmove(p,exepath,n);
    p[n]=0;
    return p;
  }
  return argv0;
#endif
#ifdef WINDOWS
  char path[PATH_MAX], *p;
  int len = GetModuleFileName(NULL, path, PATH_MAX);
  if (len > 0) {
    p = malloc(len + 1);
    memmove(p, path, len);
    p[len] = 0;
    return p;
  }
  return argv0;
#endif
}

void
usage_exit(char *herald, int exit_status, char* other_args)
{
  if (herald && *herald) {
    fprintf(dbgout, "%s\n", herald);
  }
  fprintf(dbgout, "usage: %s <options>\n", program_name);
  fprintf(dbgout, "\t or %s <image-name>\n", program_name);
  fprintf(dbgout, "\t where <options> are one or more of:\n");
  if (other_args && *other_args) {
    fputs(other_args, dbgout);
  }
  fprintf(dbgout, "\t-R, --heap-reserve <n>: reserve <n> (default: %lld)\n",
	  (u64_t) reserved_area_size);
  fprintf(dbgout, "\t\t bytes for heap expansion\n");
  fprintf(dbgout, "\t-S, --stack-size <n>: set  size of initial thread's control stack to <n>\n");
  fprintf(dbgout, "\t-Z, --thread-stack-size <n>: set default size of first (listener)  thread's stacks based on <n>\n");
  fprintf(dbgout, "\t-b, --batch: exit when EOF on *STANDARD-INPUT*\n");
  fprintf(dbgout, "\t--no-sigtrap : obscure option for running under GDB\n");
  fprintf(dbgout, "\t-I, --image-name <image-name>\n");
  fprintf(dbgout, "\t and <image-name> defaults to %s\n", 
	  default_image_name(program_name));
  fprintf(dbgout, "\n");
  _exit(exit_status);
}

int no_sigtrap = 0;
char *image_name = NULL;
int batch_flag = 0;


natural
parse_numeric_option(char *arg, char *argname, natural default_val)
{
  char *tail;
  natural val = 0;

  val = strtoul(arg, &tail, 0);
  switch(*tail) {
  case '\0':
    break;
    
  case 'M':
  case 'm':
    val = val << 20;
    break;
    
  case 'K':
  case 'k':
    val = val << 10;
    break;
    
  case 'G':
  case 'g':
    val = val << 30;
    break;
    
  default:
    fprintf(dbgout, "couldn't parse %s argument %s", argname, arg);
    val = default_val;
    break;
  }
  return val;
}
  


/* 
   The set of arguments recognized by the kernel is
   likely to remain pretty small and pretty simple.
   This removes everything it recognizes from argv;
   remaining args will be processed by lisp code.
*/

void
process_options(int argc, char *argv[])
{
  int i, j, k, num_elide, flag, arg_error;
  char *arg, *val;
#ifdef DARWIN
  extern int NXArgc;
#endif

  for (i = 1; i < argc;) {
    arg = argv[i];
    arg_error = 0;
    if (*arg != '-') {
      i++;
    } else {
      num_elide = 0;
      val = NULL;
      if ((flag = (strncmp(arg, "-I", 2) == 0)) ||
	  (strcmp (arg, "--image-name") == 0)) {
	if (flag && arg[2]) {
	  val = arg+2;
	  num_elide = 1;
	} else {
	  if ((i+1) < argc) {
	    val = argv[i+1];
	    num_elide = 2;
	  } else {
	    arg_error = 1;
	  }
	}
	if (val) {
	  image_name = val;
	}
      } else if ((flag = (strncmp(arg, "-R", 2) == 0)) ||
		 (strcmp(arg, "--heap-reserve") == 0)) {
	natural reserved_size = reserved_area_size;

	if (flag && arg[2]) {
	  val = arg+2;
	  num_elide = 1;
	} else {
	  if ((i+1) < argc) {
	    val = argv[i+1];
	    num_elide = 2;
	  } else {
	    arg_error = 1;
	  }
	}

	if (val) {
	  reserved_size = parse_numeric_option(val, 
					       "-R/--heap-reserve", 
					       reserved_area_size);
	}

	if (reserved_size <= MAXIMUM_MAPPABLE_MEMORY) {
	  reserved_area_size = reserved_size;
	}

      } else if ((flag = (strncmp(arg, "-S", 2) == 0)) ||
		 (strcmp(arg, "--stack-size") == 0)) {
	natural stack_size;

	if (flag && arg[2]) {
	  val = arg+2;
	  num_elide = 1;
	} else {
	  if ((i+1) < argc) {
	    val = argv[i+1];
	    num_elide = 2;
	  } else {
	    arg_error = 1;
	  }
	}

	if (val) {
	  stack_size = parse_numeric_option(val, 
					    "-S/--stack-size", 
					    initial_stack_size);
	  

	  if (stack_size >= MIN_CSTACK_SIZE) {
	    initial_stack_size = stack_size;
	  }
	}

      } else if ((flag = (strncmp(arg, "-Z", 2) == 0)) ||
		 (strcmp(arg, "--thread-stack-size") == 0)) {
	natural stack_size;

	if (flag && arg[2]) {
	  val = arg+2;
	  num_elide = 1;
	} else {
	  if ((i+1) < argc) {
	    val = argv[i+1];
	    num_elide = 2;
	  } else {
	    arg_error = 1;
	  }
	}

	if (val) {
	  stack_size = parse_numeric_option(val, 
					    "-Z/--thread-stack-size", 
					    thread_stack_size);
	  

	  if (stack_size >= MIN_CSTACK_SIZE) {
	   thread_stack_size = stack_size;
	  }
          if (thread_stack_size >= (1LL<<((WORD_SIZE-fixnumshift)-1))) {
            thread_stack_size = (1LL<<((WORD_SIZE-fixnumshift)-1))-1;
          }
          
	}

      } else if (strcmp(arg, "--no-sigtrap") == 0) {
	no_sigtrap = 1;
	num_elide = 1;
      } else if ((strcmp(arg, "-b") == 0) ||
		 (strcmp(arg, "--batch") == 0)) {
	batch_flag = 1;
	num_elide = 1;
      } else if (strcmp(arg,"--") == 0) {
        break;
      } else {
	i++;
      }
      if (arg_error) {
	usage_exit("error in program arguments", 1, "");
      }
      if (num_elide) {
	for (j = i+num_elide, k=i; j < argc; j++, k++) {
	  argv[k] = argv[j];
	}
	argc -= num_elide;
#ifdef DARWIN
	NXArgc -= num_elide;
#endif
	argv[argc] = NULL;
      }
    }
  }
}

#ifdef WINDOWS
void
terminate_lisp()
{
  _exit(EXIT_FAILURE);
}
#else
pid_t main_thread_pid = (pid_t)0;

void
terminate_lisp()
{
  kill(main_thread_pid, SIGKILL);
  _exit(-1);
}
#endif

#ifdef DARWIN
#define min_os_version "8.0"    /* aka Tiger */
#endif
#ifdef LINUX
#ifdef PPC
#define min_os_version "2.2"
#endif
#ifdef X86
#define min_os_version "2.6"
#endif
#endif
#ifdef FREEBSD
#define min_os_version "6.0"
#endif
#ifdef SOLARIS
#define min_os_version "5.10"
#endif

#ifdef PPC
#if defined(PPC64) || !defined(DARWIN)
/* ld64 on Darwin doesn't offer anything close to reliable control
   over the layout of a program in memory.  About all that we can
   be assured of is that the canonical subprims jump table address
   (currently 0x5000) is unmapped.  Map that page, and copy the
   actual spjump table there. */


void
remap_spjump()
{
  extern opcode spjump_start, spjump_end;
  pc new,
    old = &spjump_start,
    limit = &spjump_end,
    work;
  opcode instr;
  void *target;
  int disp;
  
  if (old != (pc)SPJUMP_TARGET_ADDRESS) {
    new = mmap((pc) SPJUMP_TARGET_ADDRESS,
               0x1000,
               PROT_READ | PROT_WRITE | PROT_EXEC,
               MAP_PRIVATE | MAP_ANON | MAP_FIXED,
               -1,
               0);
    if (new != (pc) SPJUMP_TARGET_ADDRESS) {
      perror("remap spjump");
      _exit(1);
    }
    
    for (work = new; old < limit; work++, old++) {
      instr = *old;
      disp = instr & ((1<<26)-1);
      target = (void*)old+disp;
      disp = target-(void *)work;
      *work = ((instr >> 26) << 26) | disp;
    }
    xMakeDataExecutable(new, (void*)work-(void*)new);
    ProtectMemory(new, 0x1000);
  }
}
#endif
#endif

#ifdef X86
#ifdef WINDOWS

/* By using linker tricks, we ensure there's memory between 0x11000
   and 0x21000, so we just need to fix permissions and copy the spjump
   table. */

void
remap_spjump()
{
  extern opcode spjump_start;
  DWORD old_protect;

  if ((void *)(&spjump_start) != (void *) SPJUMP_TARGET_ADDRESS) {
    if (!VirtualProtect((pc) SPJUMP_TARGET_ADDRESS,
                        0x1000,
                        PAGE_EXECUTE_READWRITE,
                        &old_protect)) {
      wperror("VirtualProtect spjump");
      _exit(1);
    }
    memmove((pc) SPJUMP_TARGET_ADDRESS, &spjump_start, 0x1000);
  }
}
#else
void
remap_spjump()
{
  extern opcode spjump_start;
  pc new = mmap((pc) SPJUMP_TARGET_ADDRESS,
                0x1000,
                PROT_READ | PROT_WRITE | PROT_EXEC,
                MAP_PRIVATE | MAP_ANON | MAP_FIXED,
                -1,
                0),
    old = &spjump_start;
  if (new == (pc)-1) {
    perror("remap spjump");
    _exit(1);
  }
  memmove(new, old, 0x1000);
}
#endif
#endif


void
check_os_version(char *progname)
{
#ifdef WINDOWS
  /* We should be able to run with any version of Windows that actually gets here executing the binary, so don't do anything for now. */
#else
  struct utsname uts;
  long got, want;
  char *got_end,*want_end;
#ifdef X8632
  extern Boolean rcontext_readonly;
#endif

  want = strtoul(min_os_version,&want_end,10);

  uname(&uts);
  got = strtoul(uts.release,&got_end,10);
#ifdef X8632
#ifdef FREEBSD
  if (!strcmp(uts.machine,"amd64")) {
    rcontext_readonly = true;
  }
#endif
#endif
  while (got == want) {
    if (*want_end == '.') {
      want = strtoul(want_end+1,&want_end,10);
      got = 0;
      if (*got_end == '.') {
        got = strtoul(got_end+1,&got_end,10);
      } else {
        break;
      }
    } else {
      break;
    }
  }

  if (got < want) {
    fprintf(dbgout, "\n%s requires %s version %s or later; the current version is %s.\n", progname, uts.sysname, min_os_version, uts.release);
    exit(1);
  }
#ifdef PPC
#ifdef DARWIN
  {
    char *hosttype = getenv("HOSTTYPE");
    if (hosttype && !strncmp("intel", hosttype, 5)) {
      running_under_rosetta = true;
      use_mach_exception_handling = false;
      reserved_area_size = 1U << 30;
    }
  }
#endif
#endif
#endif
}

#ifdef X86
/*
  This should determine the cache block size.  It should also
  probably complain if we don't have (at least) SSE2.
*/
extern int cpuid(natural, natural*, natural*, natural*);

#define X86_FEATURE_CMOV    (1<<15)
#define X86_FEATURE_CLFLUSH (1<<19)
#define X86_FEATURE_MMX     (1<<23)
#define X86_FEATURE_SSE     (1<<25)
#define X86_FEATURE_SSE2    (1<<26)

#define X86_REQUIRED_FEATURES (X86_FEATURE_CMOV|X86_FEATURE_MMX|X86_FEATURE_SSE|X86_FEATURE_SSE2)

Boolean
check_x86_cpu()
{
  natural eax, ebx, ecx, edx;

  eax = cpuid(0, &ebx, &ecx, &edx);

  if (eax >= 1) {
    eax = cpuid(1, &ebx, &ecx, &edx);
    cache_block_size = (ebx & 0xff00) >> 5;
    if ((X86_REQUIRED_FEATURES & edx) == X86_REQUIRED_FEATURES) {
      return true;
    }
    /* It's very unlikely that SSE2 would be present and other things
       that we want wouldn't.  If they don't have MMX or CMOV either,
       might as well tell them. */
    if ((edx & X86_FEATURE_SSE2) == 0) {
      fprintf(dbgout, "This CPU doesn't support the SSE2 instruction set\n");
    }
    if ((edx & X86_FEATURE_MMX) == 0) {
      fprintf(dbgout, "This CPU doesn't support the MMX instruction set\n");
    }
    if ((edx & X86_FEATURE_CMOV) == 0) {
      fprintf(dbgout, "This CPU doesn't support the CMOV instruction\n");
    }
    
  }
  return false;
}
#endif

void
lazarus()
{
  TCR *tcr = get_tcr(false);
  if (tcr) {
    /* Some threads may be dying; no threads should be created. */
    LOCK(lisp_global(TCR_AREA_LOCK),tcr);
    tcr->vs_area->active = tcr->vs_area->high - node_size;
    tcr->save_vsp = (LispObj *)(tcr->vs_area->active);
    tcr->ts_area->active = tcr->ts_area->high;
    tcr->save_tsp = (LispObj *)(tcr->ts_area->active);
    tcr->catch_top = 0;
    tcr->db_link = 0;
    tcr->xframe = 0;
    start_lisp(tcr, 0);
  }
}

#ifdef LINUX
#ifdef X8664
#include <asm/prctl.h>
#include <sys/prctl.h>

void
ensure_gs_available(char *progname)
{
  LispObj fs_addr = 0L, gs_addr = 0L, cur_thread = (LispObj)pthread_self();
  char *gnu_get_libc_version(void);
  
  arch_prctl(ARCH_GET_GS, &gs_addr);
  arch_prctl(ARCH_GET_FS, &fs_addr);
  if ((gs_addr == cur_thread) && (fs_addr != cur_thread)) {
    fprintf(dbgout, "The installed C library - version %s - seems to be using the %%gs register for thread storage.\n\"%s\" cannot run, since it expects to be\nable to use that register for its own purposes.\n", gnu_get_libc_version(),progname);
    _exit(1);
  }
}
#endif
#endif

Boolean 
bogus_fp_exceptions = false;

typedef
float (*float_arg_returns_float)(float);

float
fcallf(float_arg_returns_float fun, float arg)
{
  return fun(arg);
}

void
check_bogus_fp_exceptions()
{
#ifdef X8664
  float asinf(float),result;
    

  natural save_mxcsr = get_mxcsr(), post_mxcsr;
  set_mxcsr(0x1f80);

  result = fcallf(asinf, 1.0);
  post_mxcsr = get_mxcsr();
  set_mxcsr(save_mxcsr);
  if (post_mxcsr & (FE_ALL_EXCEPT & (~FE_INEXACT))) {
    bogus_fp_exceptions = true;
  }
#endif
}


int
main(int argc, char *argv[]
#ifndef WINDOWS
, char *envp[], void *aux
#endif
)
{
  extern int page_size;
  natural default_g0_threshold = G0_AREA_THRESHOLD,
    default_g1_threshold = G1_AREA_THRESHOLD,
    default_g2_threshold = G2_AREA_THRESHOLD,
    lisp_heap_threshold_from_image = 0;
  Boolean egc_enabled =
#ifdef DISABLE_EGC
    false
#else
    true
#endif
    ;
  Boolean lisp_heap_threshold_set_from_command_line = false;

#ifdef PPC
  extern int altivec_present;
#endif
  extern LispObj load_image(char *);
  area *a;
  BytePtr stack_base, current_sp = (BytePtr) current_stack_pointer();
  TCR *tcr;

  dbgout = stderr;

#ifdef WINDOWS
  {
    extern void init_winsock(void);
    extern void init_windows_io(void);

    _fmode = O_BINARY;
    _setmode(1, O_BINARY);
    _setmode(2, O_BINARY);
    setvbuf(dbgout, NULL, _IONBF, 0);
    init_winsock();
    init_windows_io();
  }
#endif

  check_os_version(argv[0]);
  real_executable_name = determine_executable_name(argv[0]);
  page_size = getpagesize(); /* Implement with GetSystemInfo on Windows w/o MinGW */

  check_bogus_fp_exceptions();
#ifdef LINUX
#ifdef X8664
  ensure_gs_available(real_executable_name);
#endif
#endif
#if (defined(DARWIN) && defined(PPC64)) || (defined(LINUX) && defined(PPC))|| defined(X8664) || (defined(X8632) && !defined(DARWIN))
  remap_spjump();
#endif

#ifdef PPC
#ifdef LINUX
  {
    ElfW(auxv_t) *av = aux;
    int hwcap, done = false;
    
    if (av) {
      do {
	switch (av->a_type) {
	case AT_DCACHEBSIZE:
	  cache_block_size = av->a_un.a_val;
	  break;

	case AT_HWCAP:
	  hwcap = av->a_un.a_val;
	  altivec_present = ((hwcap & PPC_FEATURE_HAS_ALTIVEC) != 0);
	  break;

	case AT_NULL:
	  done = true;
	  break;
	}
	av++;
      } while (!done);
    }
  }
#endif
#ifdef DARWIN
  {
    unsigned value = 0;
    size_t len = sizeof(value);
    int mib[2];
    
    mib[0] = CTL_HW;
    mib[1] = HW_CACHELINE;
    if (sysctl(mib,2,&value,&len, NULL, 0) != -1) {
      if (len == sizeof(value)) {
	cache_block_size = value;
      }
    }
    mib[1] = HW_VECTORUNIT;
    value = 0;
    len = sizeof(value);
    if (sysctl(mib,2,&value,&len, NULL, 0) != -1) {
      if (len == sizeof(value)) {
	altivec_present = value;
      }
    }
  }
#endif
#endif

#ifdef X86
  if (!check_x86_cpu()) {
    fprintf(dbgout, "CPU doesn't support required features\n");
    exit(1);
  }
#endif

#ifdef SOLARIS
#ifdef X8632
  {
    extern void solaris_ldt_init(void);
    solaris_ldt_init();
  }
#endif
#endif

#ifndef WINDOWS
  main_thread_pid = getpid();
#endif
  tcr_area_lock = (void *)new_recursive_lock();

  program_name = argv[0];
  if ((argc == 2) && (*argv[1] != '-')) {
    image_name = argv[1];
    argv[1] = NULL;
  } else {
    process_options(argc,argv);
  }
  if (lisp_heap_gc_threshold != DEFAULT_LISP_HEAP_GC_THRESHOLD) {
    lisp_heap_threshold_set_from_command_line = true;
  }

  initial_stack_size = ensure_stack_limit(initial_stack_size);
  if (image_name == NULL) {
    if (check_for_embedded_image(real_executable_name)) {
      image_name = real_executable_name;
    } else {
      image_name = default_image_name(real_executable_name);
    }
  }

  while (1) {
    if (create_reserved_area(reserved_area_size)) {
      break;
    }
    reserved_area_size = reserved_area_size *.9;
  }

  gc_init();

  set_nil(load_image(image_name));
  lisp_heap_threshold_from_image = lisp_global(LISP_HEAP_THRESHOLD);
  if (lisp_heap_threshold_from_image) {
    if ((!lisp_heap_threshold_set_from_command_line) &&
        (lisp_heap_threshold_from_image != lisp_heap_gc_threshold)) {
      lisp_heap_gc_threshold = lisp_heap_threshold_from_image;
      resize_dynamic_heap(active_dynamic_area->active,lisp_heap_gc_threshold);
    }
    /* If lisp_heap_threshold_from_image was set, other image params are
       valid. */
    default_g0_threshold = lisp_global(G0_THRESHOLD);
    default_g1_threshold = lisp_global(G1_THRESHOLD);
    default_g2_threshold = lisp_global(G2_THRESHOLD);
    egc_enabled = lisp_global(EGC_ENABLED);
  }

  lisp_global(TCR_AREA_LOCK) = ptr_to_lispobj(tcr_area_lock);

#ifdef X86
  lisp_global(SUBPRIMS_BASE) = (LispObj)((1<<16)+(5<<10));
#else
  lisp_global(SUBPRIMS_BASE) = (LispObj)(5<<10);
#endif
  lisp_global(RET1VALN) = (LispObj)&ret1valn;
  lisp_global(LEXPR_RETURN) = (LispObj)&nvalret;
  lisp_global(LEXPR_RETURN1V) = (LispObj)&popj;
  lisp_global(ALL_AREAS) = ptr_to_lispobj(all_areas);
#ifdef X86
  lisp_global(BAD_FUNCALL) = ptr_to_lispobj(&bad_funcall);
#endif
  lisp_global(DEFAULT_ALLOCATION_QUANTUM) = log2_heap_segment_size << fixnumshift;
  lisp_global(STACK_SIZE) = thread_stack_size<<fixnumshift;


  exception_init();

  

  lisp_global(IMAGE_NAME) = ptr_to_lispobj(image_name);
  lisp_global(ARGV) = ptr_to_lispobj(argv);
  lisp_global(KERNEL_IMPORTS) = (LispObj)import_ptrs_base;

  lisp_global(GET_TCR) = (LispObj) get_tcr;
  *(double *) &(lisp_global(DOUBLE_FLOAT_ONE)) = (double) 1.0;

  lisp_global(HOST_PLATFORM) = (LispObj) PLATFORM << fixnumshift;

  lisp_global(BATCH_FLAG) = (batch_flag << fixnumshift);

  a = active_dynamic_area;

  if (nilreg_area != NULL) {
    BytePtr lowptr = (BytePtr) a->low;

    /* Create these areas as AREA_STATIC, change them to AREA_DYNAMIC */
    g1_area = new_area(lowptr, lowptr, AREA_STATIC);
    g2_area = new_area(lowptr, lowptr, AREA_STATIC);
    tenured_area = new_area(lowptr, lowptr, AREA_STATIC);
    add_area_holding_area_lock(tenured_area);
    add_area_holding_area_lock(g2_area);
    add_area_holding_area_lock(g1_area);

    g1_area->code = AREA_DYNAMIC;
    g2_area->code = AREA_DYNAMIC;
    tenured_area->code = AREA_DYNAMIC;

/*    a->older = g1_area; */ /* Not yet: this is what "enabling the EGC" does. */
    g1_area->younger = a;
    g1_area->older = g2_area;
    g2_area->younger = g1_area;
    g2_area->older = tenured_area;
    tenured_area->younger = g2_area;
    tenured_area->refbits = a->markbits;
    tenured_area->static_dnodes = a->static_dnodes;
    a->static_dnodes = 0;
    tenured_area->static_used = a->static_used;
    a->static_used = 0;
    lisp_global(TENURED_AREA) = ptr_to_lispobj(tenured_area);
    lisp_global(REFBITS) = ptr_to_lispobj(tenured_area->refbits);
    g2_area->threshold = default_g2_threshold;
    g1_area->threshold = default_g1_threshold;
    a->threshold = default_g0_threshold;
  }

  tcr = new_tcr(initial_stack_size, MIN_TSTACK_SIZE);
  stack_base = initial_stack_bottom()-xStackSpace();
  init_threads((void *)(stack_base), tcr);
  thread_init_tcr(tcr, current_sp, current_sp-stack_base);

  lisp_global(EXCEPTION_LOCK) = ptr_to_lispobj(new_recursive_lock());
  enable_fp_exceptions();
  register_user_signal_handler();

#ifdef PPC
  lisp_global(ALTIVEC_PRESENT) = altivec_present << fixnumshift;
#endif
#if STATIC
  lisp_global(STATICALLY_LINKED) = 1 << fixnumshift;
#endif
  tcr->prev = tcr->next = tcr;
#ifndef WINDOWS
  lisp_global(INTERRUPT_SIGNAL) = (LispObj) box_fixnum(SIGNAL_FOR_PROCESS_INTERRUPT);
#endif
  tcr->vs_area->active -= node_size;
  *(--tcr->save_vsp) = nrs_TOPLFUNC.vcell;
  nrs_TOPLFUNC.vcell = lisp_nil;
#ifdef GC_INTEGRITY_CHECKING
  (nrs_GC_EVENT_STATUS_BITS.vcell |= gc_integrity_check_bit);
#endif
  if (egc_enabled) {
    egc_control(true, NULL);
  }
  atexit(lazarus);
  start_lisp(TCR_TO_TSD(tcr), 0);
  _exit(0);
}

area *
set_nil(LispObj r)
{

  if (lisp_nil == (LispObj)NULL) {

    lisp_nil = r;
  }
  return NULL;
}


void
xMakeDataExecutable(void *start, unsigned long nbytes)
{
  extern void flush_cache_lines();
  natural ustart = (natural) start, base, end;
  
  base = (ustart) & ~(cache_block_size-1);
  end = (ustart + nbytes + cache_block_size - 1) & ~(cache_block_size-1);
#ifdef DARWIN
  if (running_under_rosetta) {
    /* We probably need to flush something's cache even if running
       under Rosetta, but (a) this is agonizingly slow and (b) we're
       dying before we get to the point where this would matter.
    */
    return;
  }
#endif
#ifndef X86
  flush_cache_lines(base, (end-base)/cache_block_size, cache_block_size);
#endif
}

natural
xStackSpace()
{
  return initial_stack_size+CSTACK_HARDPROT+CSTACK_SOFTPROT;
}

#ifndef DARWIN
#ifdef WINDOWS
extern void *windows_open_shared_library(char *);

void *
xGetSharedLibrary(char *path, int mode)
{
  return windows_open_shared_library(path);
}
#else
void *
xGetSharedLibrary(char *path, int mode)
{
  return dlopen(path, mode);
}
#endif
#else
void *
xGetSharedLibrary(char *path, int *resultType)
{
#if defined(PPC) && (WORD_SIZE == 32)
  NSObjectFileImageReturnCode code;
  NSObjectFileImage	         moduleImage;
  NSModule		         module;
  const struct mach_header *     header;
  const char *                   error;
  void *                         result;
  /* not thread safe */
  /*
  static struct {
    const struct mach_header  *header;
    NSModule	              *module;
    const char                *error;
  } results;	
  */
  result = NULL;
  error = NULL;

  /* first try to open this as a bundle */
  code = NSCreateObjectFileImageFromFile(path,&moduleImage);
  if (code != NSObjectFileImageSuccess &&
      code != NSObjectFileImageInappropriateFile &&
      code != NSObjectFileImageAccess)
    {
      /* compute error strings */
      switch (code)
	{
	case NSObjectFileImageFailure:
	  error = "NSObjectFileImageFailure";
	  break;
	case NSObjectFileImageArch:
	  error = "NSObjectFileImageArch";
	  break;
	case NSObjectFileImageFormat:
	  error = "NSObjectFileImageFormat";
	  break;
	case NSObjectFileImageAccess:
	  /* can't find the file */
	  error = "NSObjectFileImageAccess";
	  break;
	default:
	  error = "unknown error";
	}
      *resultType = 0;
      return (void *)error;
    }
  if (code == NSObjectFileImageInappropriateFile ||
      code == NSObjectFileImageAccess ) {
    /* the pathname might be a partial pathane (hence the access error)
       or it might be something other than a bundle, if so perhaps
       it is a .dylib so now try to open it as a .dylib */

    /* protect against redundant loads, Gary Byers noticed possible
       heap corruption if this isn't done */
    header = NSAddImage(path, NSADDIMAGE_OPTION_RETURN_ON_ERROR |
			NSADDIMAGE_OPTION_WITH_SEARCHING |
			NSADDIMAGE_OPTION_RETURN_ONLY_IF_LOADED);
    if (!header)
      header = NSAddImage(path, NSADDIMAGE_OPTION_RETURN_ON_ERROR |
			  NSADDIMAGE_OPTION_WITH_SEARCHING);
    result = (void *)header;
    *resultType = 1;
  }
  else if (code == NSObjectFileImageSuccess) {
    /* we have a sucessful module image
       try to link it, don't bind symbols privately */

    module = NSLinkModule(moduleImage, path,
			  NSLINKMODULE_OPTION_RETURN_ON_ERROR | NSLINKMODULE_OPTION_BINDNOW);
    NSDestroyObjectFileImage(moduleImage);	
    result = (void *)module;
    *resultType = 2;
  }
  if (!result)
    {
      /* compute error string */
      NSLinkEditErrors ler;
      int lerno;
      const char* file;
      NSLinkEditError(&ler,&lerno,&file,&error);
      if (error) {
	result = (void *)error;
	*resultType = 0;
      }
    }
  return result;
#else
  const char *                   error;
  void *                         result;

  result = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
  
  if (result == NULL) {
    error = dlerror();
    *resultType = 0;
    return (void *)error;
  }
  *resultType = 1;
  return result;
#endif
}
#endif



int
fd_setsize_bytes()
{
  return FD_SETSIZE/8;
}

void
do_fd_set(int fd, fd_set *fdsetp)
{
  FD_SET(fd, fdsetp);
}

void
do_fd_clr(int fd, fd_set *fdsetp)
{
  FD_CLR(fd, fdsetp);
}

int
do_fd_is_set(int fd, fd_set *fdsetp)
{
  return FD_ISSET(fd,fdsetp);
}


void
do_fd_zero(fd_set *fdsetp)
{
  FD_ZERO(fdsetp);
}

#include "image.h"



Boolean
check_for_embedded_image (char *path)
{
  int fd = open(path, O_RDONLY);
  Boolean image_is_embedded = false;

  if (fd >= 0) {
    openmcl_image_file_header h;

    if (find_openmcl_image_file_header (fd, &h)) {
      image_is_embedded = true;
    }
    close (fd);
  }
  return image_is_embedded;
}

LispObj
load_image(char *path)
{
  int fd = open(path, O_RDONLY, 0666), err;
  LispObj image_nil = 0;

  errno = 0;
  if (fd > 0) {
    openmcl_image_file_header ih;
    image_nil = load_openmcl_image(fd, &ih);
    /* We -were- using a duplicate fd to map the file; that
       seems to confuse Darwin (doesn't everything ?), so
       we'll instead keep the original file open.
    */
    err = errno;
    if (!image_nil) {
      close(fd);
    }
#ifdef WINDOWS
    /* We currently don't actually map the image, and leaving the file
       open seems to make it difficult to write to reliably. */
    if (image_nil) {
      close(fd);
    }
#endif
  } else {
    err = errno;
  }
  if (image_nil == 0) {
    if (err == 0) {
      fprintf(dbgout, "Couldn't load lisp heap image from %s\n", path);
    } else {
      fprintf(dbgout, "Couldn't load lisp heap image from %s:\n%s\n", path, strerror(err));
    }
    exit(-1);
  }
  return image_nil;
}

int
set_errno(int val)
{
  errno = val;
  return -1;
}

/* A horrible hack to allow us to initialize a JVM instance from lisp.
   On Darwin, creating a JVM instance clobbers the thread's existing
   Mach exception infrastructure, so we save and restore it here.
*/

typedef int (*jvm_initfunc)(void*,void*,void*);

int
jvm_init(jvm_initfunc f,void*arg0,void*arg1,void*arg2)
{
  int result = -1;
  TCR *tcr = get_tcr(1);
#ifdef DARWIN
  extern kern_return_t tcr_establish_lisp_exception_port(TCR *);
#endif
  
  result = f(arg0,arg1,arg2);
#ifdef DARWIN
  tcr_establish_lisp_exception_port(tcr);
#endif
  return result;
}
  



void *
xFindSymbol(void* handle, char *name)
{
#if defined(LINUX) || defined(FREEBSD) || defined(SOLARIS)
  return dlsym(handle, name);
#endif
#ifdef DARWIN
#if defined(PPC64) || defined(X86)
  if ((handle == NULL) || (handle == ((void *) -1))) {
    handle = RTLD_DEFAULT;
  }    
  if (*name == '_') {
    name++;
  }
  return dlsym(handle, name);
#else
  natural address = 0;

  if ((handle == NULL) ||
      (handle == (void *)-1) ||
      (handle == (void *)-2)){
    if (NSIsSymbolNameDefined(name)) { /* Keep dyld_lookup from crashing */
      _dyld_lookup_and_bind(name, (void *) &address, (void*) NULL);
    }
    return (void *)address;
  }
  Bug(NULL, "How did this happen ?");
#endif
#endif
#ifdef WINDOWS
  extern void *windows_find_symbol(void *, char *);
  return windows_find_symbol(handle, name);
#endif
}

void *
get_r_debug()
{
#if defined(LINUX) || defined(FREEBSD) || defined(SOLARIS)
#if WORD_SIZE == 64
  extern Elf64_Dyn _DYNAMIC[];
  Elf64_Dyn *dp;
#else
  extern Elf32_Dyn _DYNAMIC[];
  Elf32_Dyn *dp;
#endif
  int tag;

  for (dp = _DYNAMIC; (tag = dp->d_tag) != 0; dp++) {
    if (tag == DT_DEBUG) {
      return (void *)(dp->d_un.d_ptr);
    }
  }
#endif
  return NULL;
}


#ifdef DARWIN
void
sample_paging_info(paging_info *stats)
{
  mach_msg_type_number_t count = TASK_EVENTS_INFO_COUNT;

  task_info(mach_task_self(),
            TASK_EVENTS_INFO,
            (task_info_t)stats,
            &count);
}

void
report_paging_info_delta(FILE *out, paging_info *start, paging_info *stop)
{
  fprintf(out,";;; %d soft faults, %d faults, %d pageins\n\n",
          stop->cow_faults-start->cow_faults,
          stop->faults-start->faults,
          stop->pageins-start->pageins);
}

#else
#ifdef WINDOWS
void
sample_paging_info(paging_info *stats)
{
}

void
report_paging_info_delta(FILE *out, paging_info *start, paging_info *stop)
{
}
#else
void
sample_paging_info(paging_info *stats)
{
  getrusage(RUSAGE_SELF, stats);
}

void
report_paging_info_delta(FILE *out, paging_info *start, paging_info *stop)
{
  fprintf(out,";;; %ld soft faults, %ld faults, %ld pageins\n\n",
          stop->ru_minflt-start->ru_minflt,
          stop->ru_majflt-start->ru_majflt,
          stop->ru_nswap-start->ru_nswap);
}

#endif
#endif
