#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "fcntl.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"

struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void
proc_mapstacks(pagetable_t kpgtbl)
{
  struct proc *p;
  
  for(p = proc; p < &proc[NPROC]; p++) {
    char *pa = kalloc();
    if(pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int) (p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// initialize the proc table.
void
procinit(void)
{
  struct proc *p;
  
  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  for(p = proc; p < &proc[NPROC]; p++) {
      initlock(&p->lock, "proc");
      p->state = UNUSED;
      p->kstack = KSTACK((int) (p - proc));
      p->mmap_base = TRAPFRAME;
      memset(p->vmas, 0, sizeof(p->vmas));
  }
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int
cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu*
mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc*
myproc(void)
{
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int
allocpid()
{
  int pid;
  
  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == UNUSED) {
      goto found;
    } else {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->state = USED;
  p->mmap_base = TRAPFRAME;
  memset(p->vmas, 0, sizeof(p->vmas));

  // Allocate a trapframe page.
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if(p->pagetable == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;

  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  if(p->trapframe)
    kfree((void*)p->trapframe);
  p->trapframe = 0;
  if(p->pagetable)
    proc_freepagetable(p, p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;
  p->mmap_base = TRAPFRAME;
  memset(p->vmas, 0, sizeof(p->vmas));
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;
}

static void
mmap_clear_vma(struct vma *v)
{
  memset(v, 0, sizeof(*v));
}

static int
mmap_writeback(struct vma *v, pagetable_t pagetable, uint64 addr, uint64 len)
{
  uint64 end = addr + len;

  if((v->flags & MAP_SHARED) == 0)
    return 0;

  for(uint64 a = addr; a < end; a += PGSIZE){
    pte_t *pte = walk(pagetable, a, 0);
    uint64 fileoff, n, pa;

    if(pte == 0 || (*pte & PTE_V) == 0)
      continue;

    fileoff = v->off + (a - v->addr);
    if(fileoff >= v->filelen)
      continue;
    n = PGSIZE;
    if(fileoff + n > v->filelen)
      n = v->filelen - fileoff;

    pa = PTE2PA(*pte);
    begin_op();
    ilock(v->file->ip);
    if(writei(v->file->ip, 0, pa, fileoff, n) < 0){
      iunlock(v->file->ip);
      end_op();
      return -1;
    }
    iunlock(v->file->ip);
    end_op();
  }

  return 0;
}

void
proc_mmapcleanup(struct proc *p)
{
  for(int i = 0; i < NELEM(p->vmas); i++){
    struct vma *v = &p->vmas[i];

    if(v->valid == 0 || v->file == 0)
      continue;
    mmap_writeback(v, p->pagetable, v->addr, v->len);
    fileclose(v->file);
    v->file = 0;
  }
}

uint64
proc_mmap(struct proc *p, struct file *f, uint64 length, int prot, int flags, uint64 offset)
{
  struct vma *v = 0;
  uint64 addr, size, filelen;
  int perm = PTE_U;
  int slot = -1;
  int mapped = 0;

  if(offset != 0 || length == 0)
    return -1;
  if(flags != MAP_SHARED && flags != MAP_PRIVATE)
    return -1;
  if((prot & (PROT_READ | PROT_WRITE | PROT_EXEC)) == 0)
    return -1;
  if((prot & PROT_READ) && !f->readable)
    return -1;
  if((prot & PROT_WRITE) && flags == MAP_SHARED && !f->writable)
    return -1;
  if(f->type != FD_INODE || f->ip == 0)
    return -1;

  for(int i = 0; i < NELEM(p->vmas); i++){
    if(p->vmas[i].valid == 0){
      slot = i;
      break;
    }
  }
  if(slot < 0)
    return -1;

  size = PGROUNDUP(length);
  addr = PGROUNDDOWN(p->mmap_base - size);
  if(addr < PGROUNDUP(p->sz))
    return -1;

  filedup(f);
  ilock(f->ip);
  filelen = f->ip->size;
  iunlock(f->ip);

  if(prot & PROT_READ)
    perm |= PTE_R;
  if(prot & PROT_WRITE)
    perm |= PTE_W;
  if(prot & PROT_EXEC)
    perm |= PTE_X;

  v = &p->vmas[slot];
  v->valid = 1;
  v->addr = addr;
  v->len = size;
  v->off = offset;
  v->filelen = filelen;
  v->prot = prot;
  v->flags = flags;
  v->file = f;

  for(uint64 a = 0; a < size; a += PGSIZE){
    char *mem = kalloc();
    if(mem == 0)
      goto bad;
    memset(mem, 0, PGSIZE);

    ilock(f->ip);
    if(readi(f->ip, 0, (uint64)mem, offset + a, PGSIZE) < 0){
      iunlock(f->ip);
      kfree(mem);
      goto bad;
    }
    iunlock(f->ip);

    if(mappages(p->pagetable, addr + a, PGSIZE, (uint64)mem, perm) < 0){
      kfree(mem);
      goto bad;
    }
    mapped += 1;
  }

  p->mmap_base = addr;
  return addr;

bad:
  if(mapped > 0)
    uvmunmap(p->pagetable, addr, mapped, 1);
  if(v != 0){
    fileclose(v->file);
    mmap_clear_vma(v);
  } else {
    fileclose(f);
  }
  return -1;
}

int
proc_munmap(struct proc *p, uint64 addr, uint64 length)
{
  uint64 len = PGROUNDUP(length);

  if(addr % PGSIZE != 0 || len == 0)
    return -1;

  for(int i = 0; i < NELEM(p->vmas); i++){
    struct vma *v = &p->vmas[i];
    uint64 end;

    if(v->valid == 0)
      continue;

    end = v->addr + v->len;
    if(addr == v->addr && len <= v->len){
      if(mmap_writeback(v, p->pagetable, addr, len) < 0)
        return -1;
      uvmunmap(p->pagetable, addr, len / PGSIZE, 1);
      if(len == v->len){
        fileclose(v->file);
        mmap_clear_vma(v);
      } else {
        v->addr += len;
        v->off += len;
        v->len -= len;
      }
      return 0;
    }

    if(addr + len == end && len <= v->len){
      if(mmap_writeback(v, p->pagetable, addr, len) < 0)
        return -1;
      uvmunmap(p->pagetable, addr, len / PGSIZE, 1);
      v->len -= len;
      if(v->len == 0){
        fileclose(v->file);
        mmap_clear_vma(v);
      }
      return 0;
    }

    if(addr == v->addr && len == v->len){
      if(mmap_writeback(v, p->pagetable, addr, len) < 0)
        return -1;
      uvmunmap(p->pagetable, addr, len / PGSIZE, 1);
      fileclose(v->file);
      mmap_clear_vma(v);
      return 0;
    }
  }

  return -1;
}

int
proc_mmapfork(struct proc *p, struct proc *np)
{
  np->mmap_base = p->mmap_base;
  memset(np->vmas, 0, sizeof(np->vmas));

  for(int i = 0; i < NELEM(p->vmas); i++){
    struct vma *src = &p->vmas[i];
    struct vma *dst;
    int perm = PTE_U;

    if(src->valid == 0)
      continue;

    dst = 0;
    for(int j = 0; j < NELEM(np->vmas); j++){
      if(np->vmas[j].valid == 0){
        dst = &np->vmas[j];
        break;
      }
    }
    if(dst == 0)
      return -1;

    *dst = *src;
    dst->file = filedup(src->file);

    if(src->prot & PROT_READ)
      perm |= PTE_R;
    if(src->prot & PROT_WRITE)
      perm |= PTE_W;
    if(src->prot & PROT_EXEC)
      perm |= PTE_X;

    for(uint64 a = 0; a < src->len; a += PGSIZE){
      char *mem = kalloc();
      uint64 pa = walkaddr(p->pagetable, src->addr + a);

      if(mem == 0 || pa == 0){
        if(mem)
          kfree(mem);
        return -1;
      }
      memmove(mem, (void *)pa, PGSIZE);
      if(mappages(np->pagetable, src->addr + a, PGSIZE, (uint64)mem, perm) < 0){
        kfree(mem);
        return -1;
      }
    }
  }

  return 0;
}

// Create a user page table for a given process, with no user memory,
// but with trampoline and trapframe pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if(pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if(mappages(pagetable, TRAMPOLINE, PGSIZE,
              (uint64)trampoline, PTE_R | PTE_X) < 0){
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe page just below the trampoline page, for
  // trampoline.S.
  if(mappages(pagetable, TRAPFRAME, PGSIZE,
              (uint64)(p->trapframe), PTE_R | PTE_W) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freepagetable(struct proc *p, pagetable_t pagetable, uint64 sz)
{
  for(int i = 0; i < NELEM(p->vmas); i++){
    struct vma *v = &p->vmas[i];

    if(v->valid == 0)
      continue;
    uvmunmap(pagetable, v->addr, v->len / PGSIZE, 1);
    mmap_clear_vma(v);
  }

  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// a user program that calls exec("/init")
// assembled from ../user/initcode.S
// od -t xC ../user/initcode
uchar initcode[] = {
  0x17, 0x05, 0x00, 0x00, 0x13, 0x05, 0x45, 0x02,
  0x97, 0x05, 0x00, 0x00, 0x93, 0x85, 0x35, 0x02,
  0x93, 0x08, 0x70, 0x00, 0x73, 0x00, 0x00, 0x00,
  0x93, 0x08, 0x20, 0x00, 0x73, 0x00, 0x00, 0x00,
  0xef, 0xf0, 0x9f, 0xff, 0x2f, 0x69, 0x6e, 0x69,
  0x74, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00
};

// Set up first user process.
void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;
  
  // allocate one user page and copy initcode's instructions
  // and data into it.
  uvmfirst(p->pagetable, initcode, sizeof(initcode));
  p->sz = PGSIZE;

  // prepare for the very first "return" from kernel to user.
  p->trapframe->epc = 0;      // user program counter
  p->trapframe->sp = PGSIZE;  // user stack pointer

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  p->state = RUNNABLE;

  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint64 sz;
  struct proc *p = myproc();

  sz = p->sz;
  if(n > 0){
    if((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0) {
      return -1;
    }
  } else if(n < 0){
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy user memory from parent to child.
  if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  if(proc_mmapfork(p, np) < 0){
    release(&np->lock);
    proc_mmapcleanup(np);
    freeproc(np);
    return -1;
  }

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE;
  release(&np->lock);

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void
reparent(struct proc *p)
{
  struct proc *pp;

  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->parent == p){
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
exit(int status)
{
  struct proc *p = myproc();

  if(p == initproc)
    panic("init exiting");

  // Close all open files.
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  proc_mmapcleanup(p);

  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);
  
  acquire(&p->lock);

  p->xstate = status;
  p->state = ZOMBIE;

  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(uint64 addr)
{
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(pp = proc; pp < &proc[NPROC]; pp++){
      if(pp->parent == p){
        // make sure the child isn't still in exit() or swtch().
        acquire(&pp->lock);

        havekids = 1;
        if(pp->state == ZOMBIE){
          // Found one.
          pid = pp->pid;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&pp->xstate,
                                  sizeof(pp->xstate)) < 0) {
            release(&pp->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(pp);
          release(&pp->lock);
          release(&wait_lock);
          return pid;
        }
        release(&pp->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || killed(p)){
      release(&wait_lock);
      return -1;
    }
    
    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  
  c->proc = 0;
  for(;;){
    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on();

    for(p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      if(p->state == RUNNABLE) {
        // Switch to chosen process.  It is the process's job
        // to release its lock and then reacquire it
        // before jumping back to us.
        p->state = RUNNING;
        c->proc = p;
        swtch(&c->context, &p->context);

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;
      }
      release(&p->lock);
    }
  }
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&p->lock))
    panic("sched p->lock");
  if(mycpu()->noff != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  static int first = 1;

  // Still holding p->lock from scheduler.
  release(&myproc()->lock);

  if (first) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    first = 0;
    fsinit(ROOTDEV);
  }

  usertrapret();
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&p->lock);  //DOC: sleeplock1
  release(lk);

  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  release(&p->lock);
  acquire(lk);
}

// Wake up all processes sleeping on chan.
// Must be called without any p->lock.
void
wakeup(void *chan)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    if(p != myproc()){
      acquire(&p->lock);
      if(p->state == SLEEPING && p->chan == chan) {
        p->state = RUNNABLE;
      }
      release(&p->lock);
    }
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kill(int pid)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      p->killed = 1;
      if(p->state == SLEEPING){
        // Wake process from sleep().
        p->state = RUNNABLE;
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

void
setkilled(struct proc *p)
{
  acquire(&p->lock);
  p->killed = 1;
  release(&p->lock);
}

int
killed(struct proc *p)
{
  int k;
  
  acquire(&p->lock);
  k = p->killed;
  release(&p->lock);
  return k;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if(user_dst){
    return copyout(p->pagetable, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if(user_src){
    return copyin(p->pagetable, dst, src, len);
  } else {
    memmove(dst, (char*)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [USED]      "used",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  struct proc *p;
  char *state;

  printf("\n");
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s", p->pid, state, p->name);
    printf("\n");
  }
}
