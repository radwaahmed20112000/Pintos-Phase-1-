#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/switch.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "devices/timer.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif


/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list ready_list;

/* List of all processes.  Processes are added to this list
   when they are first scheduled and removed when they exit. */
static struct list all_list;

/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Stack frame for kernel_thread(). */
struct kernel_thread_frame 
  {
    void *eip;                  /* Return address. */
    thread_func *function;      /* Function to call. */
    void *aux;                  /* Auxiliary data for function. */
  };

/* Statistics. */
static long long idle_ticks;    /* # of timer ticks spent idle. */
static long long kernel_ticks;  /* # of timer ticks in kernel threads. */
static long long user_ticks;    /* # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4            /* # of timer ticks to give each thread. */
static unsigned thread_ticks;   /* # of timer ticks since last yield. */

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;

static struct real load_avg;


static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *running_thread (void);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static bool is_thread (struct thread *) UNUSED;
static void *alloc_frame (struct thread *, size_t size);
static void schedule (void);
void thread_schedule_tail (struct thread *prev);
static tid_t allocate_tid (void);

static bool
priority_compare(const struct list_elem *a_, const struct list_elem *b_,
            void *aux UNUSED) 
{
  const struct thread *a = list_entry (a_, struct thread, elem);
  const struct thread *b = list_entry (b_, struct thread, elem);
  
  return a-> virtualPriority > b-> virtualPriority;
}
/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */
void
thread_init (void) 
{
  ASSERT (intr_get_level () == INTR_OFF);

  lock_init (&tid_lock);
  list_init (&ready_list);
  list_init (&all_list);

  /* Set up a thread structure for the running thread. */
  initial_thread = running_thread ();
  init_thread (initial_thread, "main", PRI_DEFAULT);
  initial_thread->status = THREAD_RUNNING;
  initial_thread->tid = allocate_tid ();

}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void
thread_start (void) 
{
  /* Create the idle thread. */
  struct semaphore idle_started;
  sema_init (&idle_started, 0);
  thread_create ("idle", PRI_MIN, idle, &idle_started);

  /* Start preemptive thread scheduling. */
  intr_enable ();

  /* Wait for the idle thread to initialize idle_thread. */
  sema_down (&idle_started);
}



/* Prints thread statistics. */
void
thread_print_stats (void) 
{
  printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
          idle_ticks, kernel_ticks, user_ticks);
}

/* Creates a new kernel thread named NAME with the given initial
   PRIORITY, which executes FUNCTION passing AUX as the argument,
   and adds it to the ready queue.  Returns the thread identifier
   for the new thread, or TID_ERROR if creation fails.

   If thread_start() has been called, then the new thread may be
   scheduled before thread_create() returns.  It could even exit
   before thread_create() returns.  Contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  Use a semaphore or some other form of
   synchronization if you need to ensure ordering.

   The code provided sets the new thread's `priority' member to
   PRIORITY, but no actual priority scheduling is implemented.
   Priority scheduling is the goal of Problem 1-3. */
tid_t
thread_create (const char *name, int priority,
               thread_func *function, void *aux) 
{
  struct thread *t;
  struct kernel_thread_frame *kf;
  struct switch_entry_frame *ef;
  struct switch_threads_frame *sf;
  tid_t tid;

  ASSERT (function != NULL);

  /* Allocate thread. */
  t = palloc_get_page (PAL_ZERO);
  if (t == NULL)
    return TID_ERROR;

  /* Initialize thread. */
  init_thread (t, name, priority);
  tid = t->tid = allocate_tid ();
  t->nice = thread_current()->nice;
  t->recent_cpu = thread_current()->recent_cpu;

  /* Stack frame for kernel_thread(). */
  kf = alloc_frame (t, sizeof *kf);
  kf->eip = NULL;
  kf->function = function;
  kf->aux = aux;

  /* Stack frame for switch_entry(). */
  ef = alloc_frame (t, sizeof *ef);
  ef->eip = (void (*) (void)) kernel_thread;

  /* Stack frame for switch_threads(). */
  sf = alloc_frame (t, sizeof *sf);
  sf->eip = switch_entry;
  sf->ebp = 0;

  /* Add to run queue. */
  thread_unblock (t);
  return tid;
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void
thread_block (void) 
{
  ASSERT (!intr_context ());
  ASSERT (intr_get_level () == INTR_OFF);

  thread_current ()->status = THREAD_BLOCKED;
  schedule ();
}

/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */
void
thread_unblock (struct thread *t) 
{
  enum intr_level old_level;

  ASSERT (is_thread (t));

  old_level = intr_disable ();
  ASSERT (t->status == THREAD_BLOCKED); 
  list_insert_ordered (&ready_list, &t->elem,priority_compare,NULL);
  t->status = THREAD_READY;
  thread_yield();
  intr_set_level (old_level);
}

/* Returns the name of the running thread. */
const char *
thread_name (void) 
{
  return thread_current ()->name;
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *
thread_current (void) 
{
  struct thread *t = running_thread ();
  
  /* Make sure T is really a thread.
     If either of these assertions fire, then your thread may
     have overflowed its stack.  Each thread has less than 4 kB
     of stack, so a few big automatic arrays or moderate
     recursion can cause stack overflow. */
  ASSERT (is_thread (t));
  ASSERT (t->status == THREAD_RUNNING);

  return t;
}

/* Returns the running thread's tid. */
tid_t
thread_tid (void) 
{
  return thread_current ()->tid;
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void
thread_exit (void) 
{
  ASSERT (!intr_context ());

#ifdef USERPROG
  process_exit ();
#endif

  /* Remove thread from all threads list, set our status to dying,
     and schedule another process.  That process will destroy us
     when it calls thread_schedule_tail(). */
  intr_disable ();
  list_remove (&thread_current()->allelem);
  thread_current ()->status = THREAD_DYING;
  schedule ();
  NOT_REACHED ();
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
void
thread_yield (void) 
{
  struct thread *cur = thread_current ();
  enum intr_level old_level;
  
  ASSERT (!intr_context ());

  old_level = intr_disable ();
  if (cur != idle_thread) 
    list_insert_ordered (&ready_list, &cur->elem,priority_compare,NULL);

  cur->status = THREAD_READY;
  schedule ();
  intr_set_level (old_level);
}

/* Invoke function 'func' on all threads, passing along 'aux'.
   This function must be called with interrupts off. */
void
thread_foreach (thread_action_func *func, void *aux)
{
  struct list_elem *e;

  ASSERT (intr_get_level () == INTR_OFF);

  for (e = list_begin (&all_list); e != list_end (&all_list);
       e = list_next (e))
    {
      struct thread *t = list_entry (e, struct thread, allelem);
      func (t, aux);
    }
}

/* Sets the current thread's virtual priority to NEW_PRIORITY. */
void
thread_set_virtual_priority (int new_priority) 
{
  thread_current ()-> virtualPriority = new_priority;
}

/* Sets the current thread's priority to NEW_PRIORITY. */
/* Sets the current thread's priority to NEW_PRIORITY. */
void
thread_set_priority (int new_priority) 
{
  thread_current ()->priority = new_priority;
  list_sort(&ready_list,&priority_compare,NULL);
  if(thread_current()->priority < list_begin(&ready_list)){
    thread_yield();
  }
}

/* Returns the current thread's priority. */
int
thread_get_priority (void) 
{
  return thread_current ()->priority;
}

/* Sets the current thread's nice value to NICE. */
void
thread_set_nice (int nice UNUSED) 
{
  thread_current()->nice = nice;
}

/* Returns the current thread's nice value. */
int
thread_get_nice (void) 
{
  struct real r ;
  r.value = thread_current()->nice;
  return  (r.value);
}

/* Returns 100 times the system load average. */
int
thread_get_load_avg (void) 
{
  struct real r = multiply_fixed_point_and_integer (load_avg ,100);
  return  convert_fixed_to_integer_nearest(r);
}

/* Returns 100 times the current thread's recent_cpu value. */
int
thread_get_recent_cpu (void) 
{
  struct real r ;
  r.value = thread_current()->recent_cpu.value;
  return  convert_fixed_to_integer_nearest(r);
}
/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void
idle (void *idle_started_ UNUSED) 
{
  struct semaphore *idle_started = idle_started_;
  idle_thread = thread_current ();
  sema_up (idle_started);

  for (;;) 
    {
      /* Let someone else run. */
      intr_disable ();
      thread_block ();

      /* Re-enable interrupts and wait for the next one.

         The `sti' instruction disables interrupts until the
         completion of the next instruction, so these two
         instructions are executed atomically.  This atomicity is
         important; otherwise, an interrupt could be handled
         between re-enabling interrupts and waiting for the next
         one to occur, wasting as much as one clock tick worth of
         time.

         See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
         7.11.1 "HLT Instruction". */
      asm volatile ("sti; hlt" : : : "memory");
    }
}

/* Function used as the basis for a kernel thread. */
static void
kernel_thread (thread_func *function, void *aux) 
{
  ASSERT (function != NULL);

  intr_enable ();       /* The scheduler runs with interrupts off. */
  function (aux);       /* Execute the thread function. */
  thread_exit ();       /* If function() returns, kill the thread. */
}

/* Returns the running thread. */
struct thread *
running_thread (void) 
{
  uint32_t *esp;

  /* Copy the CPU's stack pointer into `esp', and then round that
     down to the start of a page.  Because `struct thread' is
     always at the beginning of a page and the stack pointer is
     somewhere in the middle, this locates the curent thread. */
  asm ("mov %%esp, %0" : "=g" (esp));
  return pg_round_down (esp);
}

/* Returns true if T appears to point to a valid thread. */
static bool
is_thread (struct thread *t)
{
  return t != NULL && t->magic == THREAD_MAGIC;
}

/* Does basic initialization of T as a blocked thread named
   NAME. */
static void
init_thread (struct thread *t, const char *name, int priority)
{
  enum intr_level old_level;

  ASSERT (t != NULL);
  ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
  ASSERT (name != NULL);

  memset (t, 0, sizeof *t);
  t->status = THREAD_BLOCKED;
  strlcpy (t->name, name, sizeof t->name);
  t->stack = (uint8_t *) t + PGSIZE;
  t->priority = priority;
  t->virtualPriority = priority;
  t->magic = THREAD_MAGIC;
  list_init (&initial_thread->locks);
  old_level = intr_disable ();
  list_push_back (&all_list, &t->allelem);
  intr_set_level (old_level);
}

/* Allocates a SIZE-byte frame at the top of thread T's stack and
   returns a pointer to the frame's base. */
static void *
alloc_frame (struct thread *t, size_t size) 
{
  /* Stack data is always allocated in word-size units. */
  ASSERT (is_thread (t));
  ASSERT (size % sizeof (uint32_t) == 0);

  t->stack -= size;
  return t->stack;
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread *
next_thread_to_run (void) 
{
  if (list_empty (&ready_list))
    return idle_thread;
  else
    return list_entry (list_pop_front (&ready_list), struct thread, elem);
}

/* Completes a thread switch by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.  This function is normally invoked by
   thread_schedule() as its final action before returning, but
   the first time a thread is scheduled it is called by
   switch_entry() (see switch.S).

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function.

   After this function and its caller returns, the thread switch
   is complete. */
void
thread_schedule_tail (struct thread *prev)
{
  struct thread *cur = running_thread ();
  
  ASSERT (intr_get_level () == INTR_OFF);

  /* Mark us as running. */
  cur->status = THREAD_RUNNING;

  /* Start new time slice. */
  thread_ticks = 0;

#ifdef USERPROG
  /* Activate the new address space. */
  process_activate ();
#endif

  /* If the thread we switched from is dying, destroy its struct
     thread.  This must happen late so that thread_exit() doesn't
     pull out the rug under itself.  (We don't free
     initial_thread because its memory was not obtained via
     palloc().) */
  if (prev != NULL && prev->status == THREAD_DYING && prev != initial_thread) 
    {
      ASSERT (prev != cur);
      palloc_free_page (prev);
    }
}

/* Schedules a new process.  At entry, interrupts must be off and
   the running process's state must have been changed from
   running to some other state.  This function finds another
   thread to run and switches to it.

   It's not safe to call printf() until thread_schedule_tail()
   has completed. */
static void
schedule (void) 
{
  struct thread *cur = running_thread ();
  if(cur->status == THREAD_RUNNING)
    list_insert_ordered (&ready_list, &cur->elem,priority_compare,NULL);
  struct thread *next = next_thread_to_run ();
  struct thread *prev = NULL;

  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (cur->status != THREAD_RUNNING);
  ASSERT (is_thread (next));
  if (cur != next )
    prev = switch_threads (cur, next);
  thread_schedule_tail (prev);
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid (void) 
{
  static tid_t next_tid = 1;
  tid_t tid;

  lock_acquire (&tid_lock);
  tid = next_tid++;
  lock_release (&tid_lock);

  return tid;
}


/* Offset of `stack' member within `struct thread'.
   Used by switch.S, which can't figure it out on its own. */
uint32_t thread_stack_ofs = offsetof (struct thread, stack);

// Convert n to fixed point: n * f
struct real convert_integer_to_fixed_point(int n){
    struct real fixed ;
    fixed.value = n * fraction;
    return fixed;
}
// Convert x to integer (rounding toward zero): x / f
int convert_fixed_to_integer (struct real real ){
    return real.value/fraction;
} 
// Convert x to integer (rounding to nearest): (x + f / 2) / f if x >= 0,
// (x - f / 2) / f if x <= 0.
int convert_fixed_to_integer_nearest(struct real real){
    int x = real.value;
    if (x >= 0){
        return (x + fraction / 2) / fraction;
    }else{
        return (x -fraction/2)/fraction;
    }
}
// Add x and y: x + y
struct real add_two_fixed_points(struct real x , struct real y){
    struct real result;
    result.value = x.value + y.value;
    return result;
}
// Subtract y from x: x - y
struct real subtract_two_fixed_points(struct real x , struct real y){
    struct real result;
    result.value = x.value - y.value;
    return result;
}
// Add x and n: x + n * f
struct real add_fixed_point_and_integer(struct real x , int n){
    struct real result;
    result.value = x.value + n * fraction;
    return result;
}
// Subtract n from x: x - n * f
struct real subtract_fixed_point_and_integer(struct real x , int n){
    struct real result;
    result.value = n * fraction - x.value;
    return result;
}
// Multiply x by y: ((int64_t) x) * y / f
struct real multiply_two_fixed_points(struct real x , struct real y){
    struct real result;
    result.value = ((int64_t) x.value) * y.value / fraction;
    return result;
}
// Multiply x by n: x * n
struct real multiply_fixed_point_and_integer(struct real x , int n){
    struct real result;
    result.value = x.value * n ;
    return result;
}
// Divide x by y: ((int64_t) x) * f / y
struct real divide_two_fixed_points(struct real x , struct real y){
    struct real result;
    result.value = ((int64_t) x.value) * fraction / y.value;
    return result;
}
// Divide x by n: x / n
struct real divide_fixed_point_and_integer(struct real x , int n){
    struct real result;
    result.value = x.value / n;
    return result;
}/*
void
thread_update_priority(){
  struct list_elem *e;
  for (e = list_rbegin (&all_list); e != list_rend (&all_list);e = list_prev (e))
  {
    struct thread *t = list_entry (e, struct thread, elem);
    //Calculate Load Average 
    struct real r = multiply_fixed_point_and_integer(load_avg,59);
    struct real first_term = divide_fixed_point_and_integer(r,60);
    struct real term = convert_integer_to_fixed_point(length_of_ready_list());
    struct real second_term = divide_fixed_point_and_integer(term,60);
    struct real result = add_two_fixed_points(first_term,second_term);    
    load_avg =  result;
    // printf("hhhhhhhhhhhhhhhhhhhhhhhhhLOADhhhhhhhhhhhh\n");
    // printf("%d\n",t->load_avg.value);
    
    //Calculate Recent CPU 
      first_term = multiply_fixed_point_and_integer(load_avg,2);
      second_term = add_fixed_point_and_integer(first_term,1);
    struct real third_term = divide_two_fixed_points(first_term,second_term);
    struct real fourth_term = multiply_two_fixed_points(third_term,t->recent_cpu);
     result = add_two_fixed_points(fourth_term,t->nice);
    t->recent_cpu = result;
    if(thread_mlfqs){
      //Calculate Priority
       struct real first_term = divide_fixed_point_and_integer(t->recent_cpu,4);
      struct real second_term = multiply_fixed_point_and_integer(t->nice,2);
      struct real third_term = subtract_fixed_point_and_integer(first_term,PRI_MAX);
      struct real result = subtract_two_fixed_points(third_term,second_term);
      t->priority = convert_fixed_to_integer_nearest(result);
     
    }
  }
  list_sort(&ready_list,&priority_compare,NULL);
  // if(thread_current()->priority < list_begin(&ready_list)){
  //   thread_yield();
  // }
}*/


int length_of_ready_list(void){

  struct list_elem *e;
  size_t cnt = 0;
  struct thread *t;
  
  for (e = list_begin (&ready_list); e != list_end (&ready_list); e = list_next (e)){
    t = list_entry(e, struct thread, elem);
    if(t != idle_thread)
        cnt++;
  }
  
  if(running_thread() != idle_thread)
    cnt++;

   return cnt;
}
/*
void update_priority(struct thread *t){
   if(t!=idle_thread){
      struct real first_term = divide_fixed_point_and_integer(t->recent_cpu,4);
      struct real second_term = multiply_fixed_point_and_integer(t->nice,2);
      struct real third_term = subtract_fixed_point_and_integer(first_term,PRI_MAX);
      struct real result = subtract_two_fixed_points(third_term,second_term);
      t->priority = convert_fixed_to_integer_nearest(result);
   }else{
      t->priority=PRI_MAX;
     }

}*/
void update_load_avg(){

    struct real r = multiply_fixed_point_and_integer(load_avg,59);
    struct real first_term = divide_fixed_point_and_integer(r,60);
    struct real term = convert_integer_to_fixed_point(length_of_ready_list());
    struct real second_term = divide_fixed_point_and_integer(term,60);
    struct real result = add_two_fixed_points(first_term,second_term);    
    load_avg =  result;
}/*
void update_recent_cpu(struct thread *t){
    if(t==idle_thread){
    return;
  }
    struct real first_term = multiply_fixed_point_and_integer(load_avg,2);
    struct real second_term = add_fixed_point_and_integer(first_term,1);
    struct real third_term = divide_two_fixed_points(first_term,second_term);
    struct real fourth_term = multiply_two_fixed_points(third_term,t->recent_cpu);
    struct real result = add_two_fixed_points(fourth_term,t->nice);
    t->recent_cpu = result;
}
void update_all_recent_cpu(void){
  if(thread_mlfqs){

  struct list_elem *e;

  for (e = list_begin (&all_list); e != list_end (&all_list); e = list_next (e)){

    struct thread *t = list_entry(e,struct thread, allelem);


    if(t != idle_thread){
        update_recent_cpu(t);  
  }
}
}

}

void update_priority_of_all_threads_in_all_list (void)
{
  if(thread_mlfqs){
  struct list_elem *e;
  struct thread *t;

  for (e = list_rbegin (&all_list); e != list_rend (&all_list);e = list_prev (e))
  {
    struct thread *t = list_entry (e, struct thread, elem);
    if(t != idle_thread){
        update_priority(t);
    
    if (t->priority > PRI_MAX)
        t->priority = PRI_MAX;
    else if (t->priority < PRI_MIN)
        t->priority = PRI_MIN;
    }
    }
    // i must here to resort all element
    list_sort(&ready_list,&priority_compare,NULL);
  }
}*/
/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */
void
thread_tick (void) 
{
  struct thread *t = thread_current ();

  /* Update statistics. */
  if (t == idle_thread)
    idle_ticks++;
#ifdef USERPROG
  else if (t->pagedir != NULL)
    user_ticks++;
#endif
  else
    kernel_ticks++;

  /* Enforce preemption. */
  if (++thread_ticks >= TIME_SLICE)
    intr_yield_on_return ();
}
/*
  struct real one;
  struct thread *cur_thread;

  if(thread_mlfqs){
    cur_thread = thread_current();
    cur_thread->recent_cpu = add_fixed_point_and_integer(cur_thread->recent_cpu ,1);
    if (timer_ticks() % TIMER_FREQ == 0) /* do this every second *//*
    {
       update_load_avg();
       update_all_recent_cpu();
       update_priority_of_all_threads_in_all_list();

    }


    if(timer_ticks() % 4 == 0){
      struct thread *t = thread_current();
      update_priority(t);
    }

  }
  /* Enforce preemption. */ /*
  if (++thread_ticks >= TIME_SLICE)
    intr_yield_on_return ();
 // printf("end of thread tick \n");
}
}*/


void thread_interrupt(int64_t ticks){


  //if(thread_mlfqs){
    //foreach();

    //start BSD

    /* updating the recent_cpu value for all threads e`very second not every tick and then recalculate their priority value*/
    if(thread_current() != idle_thread){
      thread_current()->recent_cpu = add_fixed_point_and_integer(thread_current()->recent_cpu, 1);
    }

    /* updating the load average value every second. */

    //if(ticks % TIMER_FREQ == 0){
      int ready_running_threads = list_empty(&ready_list) ? 0 : list_size(&ready_list);
      if(thread_current() != idle_thread) ready_running_threads++;

      /* load_avg = (59/60)*load_avg + (1/60)*ready_threads, */
      load_avg = add_two_fixed_points(divide_fixed_point_and_integer(multiply_fixed_point_and_integer(load_avg, 59), 60) ,
                    divide_fixed_point_and_integer(convert_integer_to_fixed_point(ready_running_threads), 60));
   // }


    /* updating the recent_cpu value for all threads every second not every tick and then recalculate their priority value*/
    if(timer_ticks () % TIMER_FREQ == 0){
      struct list_elem *e;
      struct thread *t;
      for(e = list_begin(&all_list); e != list_end(&all_list); e = list_next(e)){
          t = list_entry(e, struct thread, allelem);
          if(t == idle_thread) continue;

          /* recent_cpu = (2*load_avg)/(2*load_avg + 1) * recent_cpu + nice. */
          t->recent_cpu = add_fixed_point_and_integer(multiply_two_fixed_points(divide_two_fixed_points(multiply_fixed_point_and_integer(load_avg, 2),
                            add_fixed_point_and_integer(multiply_fixed_point_and_integer(load_avg, 2), 1)) , t->recent_cpu) , t->nice);
      }
    }
if(thread_mlfqs){
    if(timer_ticks() % 4 == 0){
      struct list_elem *e;
      int ind = 0;
      for(e = list_begin(&all_list); e != list_end(&all_list); e = list_next(e)){
          struct thread *t = list_entry(e, struct thread, allelem);
          if(t == idle_thread) continue;

          /*  priority = PRI_MAX - (recent_cpu / 4) - (nice * 2). */
          t->priority = PRI_MAX - convert_fixed_to_integer_nearest(divide_fixed_point_and_integer(t->recent_cpu, 4)) - (t->nice * 2);
          t->priority = (t->priority > PRI_MAX) ? PRI_MAX : (t->priority < PRI_MIN ? PRI_MIN : t->priority) ;

          if(e == NULL)break;
      }
      if(!list_empty(&ready_list))
        list_sort(&ready_list, priority_compare, NULL);
    }
  }

  thread_tick();
  //foreach();
}