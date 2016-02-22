#include "devices/timer.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include "devices/pit.h"
#include "threads/interrupt.h"
#include "threads/synch.h"
#include "threads/thread.h"
  
/* See [8254] for hardware details of the 8254 timer chip. */

#if TIMER_FREQ < 19
#error 8254 timer requires TIMER_FREQ >= 19
#endif
#if TIMER_FREQ > 1000
#error TIMER_FREQ <= 1000 recommended
#endif

/* Number of timer ticks since OS booted. */
static int64_t ticks;

/* Number of loops per timer tick.
   Initialized by timer_calibrate(). */
static unsigned loops_per_tick;

static intr_handler_func timer_interrupt;

// added code---------------------------------------------------------------------------------
struct sleeper {
  int64_t sleep_til; /* the tick count when the sleeper shoudl wake */
  struct semaphore sema;

  //required for list
  struct list_elem elem;
};

static struct semaphore sleep_list_sema; /* control access to sleep list */

static struct list sleep_list;    /* list of sleepers to be checked*/


bool timer_less (const struct list_elem *a, const struct list_elem *b, void *aux){
  struct sleeper* snp1 = list_entry(a, struct sleeper, elem);
  struct sleeper* snp2 = list_entry(b, struct sleeper, elem);
  printf("[%d] %ld < %ld = %d\n", (int)thread_tid(), (long)snp1->sleep_til, (long)snp2->sleep_til, (int)(snp1->sleep_til < snp2->sleep_til));
  ASSERT(snp1 != snp2);
  return (snp1->sleep_til < snp2->sleep_til);
}

void insert_sleeper(struct list_elem* list_element){
  ASSERT(list_element);
  printf("list is %d long\n", list_size(&sleep_list));
  list_insert_ordered (&sleep_list, list_element, timer_less, NULL);
}

void timer_tick () {
        // first element should be lowest, since less fn applied
  if( list_size(&sleep_list) > 0 ) {
        struct list_elem* e= list_front(&sleep_list);
        struct sleeper* sp = list_entry(e, struct sleeper, elem);
        ASSERT( (sp->sleep_til));
        if( sp->sleep_til <= ticks ){
          //printf("!!!! %ld <= %ld? \n", (long)(sp->sleep_til), (long)ticks);
          //remove from list
          list_pop_front(&sleep_list);
          //let the thread resume
          sema_up(&(sp->sema));
        }
  }
}

// ----------------------------------------------------------------------------------------

static bool too_many_loops (unsigned loops);
static void busy_wait (int64_t loops);
static void real_time_sleep (int64_t num, int32_t denom);
static void real_time_delay (int64_t num, int32_t denom);

/* Sets up the timer to interrupt TIMER_FREQ times per second,
   and registers the corresponding interrupt. */ 
void
timer_init (void) 
{
  // added code---------------------------------------------------------------------------------
    // -- init sleeper list
  printf("timer_init is running\n");
  list_init(&sleep_list);
    // -- init list control semaphore
  sema_init(&sleep_list_sema, 1);
  // ----------------------------------------------------------------------------------------
  pit_configure_channel (0, 2, TIMER_FREQ);
  intr_register_ext (0x20, timer_interrupt, "8254 Timer");
}

/* Calibrates loops_per_tick, used to implement brief delays. */
void
timer_calibrate (void) 
{
  unsigned high_bit, test_bit;

  ASSERT (intr_get_level () == INTR_ON);
  printf ("Calibrating timer...  ");

  /* Approximate loops_per_tick as the largest power-of-two
     still less than one timer tick. */
  loops_per_tick = 1u << 10;
  while (!too_many_loops (loops_per_tick << 1)) 
    {
      loops_per_tick <<= 1;
      ASSERT (loops_per_tick != 0);
    }

  /* Refine the next 8 bits of loops_per_tick. */
  high_bit = loops_per_tick;
  for (test_bit = high_bit >> 1; test_bit != high_bit >> 10; test_bit >>= 1)
    if (!too_many_loops (high_bit | test_bit))
      loops_per_tick |= test_bit;

  printf ("%'"PRIu64" loops/s.\n", (uint64_t) loops_per_tick * TIMER_FREQ);
}

/* Returns the number of timer ticks since the OS booted. */
int64_t
timer_ticks (void) 
{
  enum intr_level old_level = intr_disable ();
  int64_t t = ticks;
  intr_set_level (old_level);
  return t; 
}

/* Returns the number of timer ticks elapsed since THEN, which
   should be a value once returned by timer_ticks(). */
int64_t
timer_elapsed (int64_t then) 
{
  return timer_ticks () - then;
}

/* Sleeps for approximately TICKS timer ticks.  Interrupts must
   be turned on. */
void
//timer_sleep (int64_t ticks) 
timer_sleep (int64_t _ticks) 
{
  // per-thread struct
  struct sleeper sleep_node;
  // init sleep node sema
  sema_init(&(sleep_node.sema),0);

  int thread_id = (int)thread_tid();
  int64_t start = ticks;
  int64_t stop = start + _ticks; 
  //printf ("putting thread %d to sleep for %ld ticks at %ld to %ld\n", thread_id, (long)_ticks, (long)start, (long)stop);
  //printf ("sleep_node ptr: %p\n",&sleep_node);
  ASSERT (intr_get_level () == INTR_ON);

  // --------------------added code--------------------------------------------------------------

  // no interrupts?
  //    initialize a sleeper
  sleep_node.sleep_til = (int64_t)stop;
  //  sema_init(&(sleep_node.sema),0); <-- moved to timer_init
  ASSERT(!(sleep_node.sema.value));
  //printf("created sleeper at %ld ticks til %ld ticks\n", (long)start, (long)sleep_node.sleep_til);

  //sema_down(&sleep_list_sema);   /* acquire rights to list */
  enum intr_level old_level = intr_disable();
  insert_sleeper(&(sleep_node.elem));
  //printf("inserted element into list [%d]\n", list_size(&sleep_list));
  //sema_up(&sleep_list_sema);    /* release hold on sleep list */
  //printf("sema'd up\n");
  intr_set_level (old_level);
  // call the node's sema down, making it block until it is woken by the timing coordinator
  //printf("sleeping via sema_down");
  sema_down(&(sleep_node.sema));
  // block thread
  // resume when condition met


  // --------------------------------------------------------------------------------------------
  /*

  
  // busy waiting
  while (timer_elapsed (start) < _ticks) 
    thread_yield ();
//*/
  //printf("thread %d timer_sleep() end at t=%ld\n", thread_id, (long)ticks);
  ASSERT (intr_get_level () == INTR_ON);
}

/* Sleeps for approximately MS milliseconds.  Interrupts must be
   turned on. */
void
timer_msleep (int64_t ms) 
{
  real_time_sleep (ms, 1000);
}

/* Sleeps for approximately US microseconds.  Interrupts must be
   turned on. */
void
timer_usleep (int64_t us) 
{
  real_time_sleep (us, 1000 * 1000);
}

/* Sleeps for approximately NS nanoseconds.  Interrupts must be
   turned on. */
void
timer_nsleep (int64_t ns) 
{
  real_time_sleep (ns, 1000 * 1000 * 1000);
}

/* Busy-waits for approximately MS milliseconds.  Interrupts need
   not be turned on.

   Busy waiting wastes CPU cycles, and busy waiting with
   interrupts off for the interval between timer ticks or longer
   will cause timer ticks to be lost.  Thus, use timer_msleep()
   instead if interrupts are enabled. */
void
timer_mdelay (int64_t ms) 
{
  real_time_delay (ms, 1000);
}

/* Sleeps for approximately US microseconds.  Interrupts need not
   be turned on.

   Busy waiting wastes CPU cycles, and busy waiting with
   interrupts off for the interval between timer ticks or longer
   will cause timer ticks to be lost.  Thus, use timer_usleep()
   instead if interrupts are enabled. */
void
timer_udelay (int64_t us) 
{
  real_time_delay (us, 1000 * 1000);
}

/* Sleeps execution for approximately NS nanoseconds.  Interrupts
   need not be turned on.

   Busy waiting wastes CPU cycles, and busy waiting with
   interrupts off for the interval between timer ticks or longer
   will cause timer ticks to be lost.  Thus, use timer_nsleep()
   instead if interrupts are enabled.*/
void
timer_ndelay (int64_t ns) 
{
  real_time_delay (ns, 1000 * 1000 * 1000);
}

/* Prints timer statistics. */
void
timer_print_stats (void) 
{
  printf ("Timer: %"PRId64" ticks\n", timer_ticks ());
}

/* Timer interrupt handler. */
static void
timer_interrupt (struct intr_frame *args UNUSED)
{
  ticks++;
  thread_tick ();
  // ------------ added code --------------------------------------
  if(ticks%2)
    timer_tick ();
  // --------------------------------------------------------------
}

/* Returns true if LOOPS iterations waits for more than one timer
   tick, otherwise false. */
static bool
too_many_loops (unsigned loops) 
{
  /* Wait for a timer tick. */
  int64_t start = ticks;
  while (ticks == start)
    barrier ();

  /* Run LOOPS loops. */
  start = ticks;
  busy_wait (loops);

  /* If the tick count changed, we iterated too long. */
  barrier ();
  return start != ticks;
}

/* Iterates through a simple loop LOOPS times, for implementing
   brief delays.

   Marked NO_INLINE because code alignment can significantly
   affect timings, so that if this function was inlined
   differently in different places the results would be difficult
   to predict. */
static void NO_INLINE
busy_wait (int64_t loops) 
{
  while (loops-- > 0)
    barrier ();
}

/* Sleep for approximately NUM/DENOM seconds. */
static void
real_time_sleep (int64_t num, int32_t denom) 
{
  /* Convert NUM/DENOM seconds into timer ticks, rounding down.
          
        (NUM / DENOM) s          
     ---------------------- = NUM * TIMER_FREQ / DENOM ticks. 
     1 s / TIMER_FREQ ticks
  */
  int64_t ticks = num * TIMER_FREQ / denom;

  ASSERT (intr_get_level () == INTR_ON);
  if (ticks > 0)
    {
      /* We're waiting for at least one full timer tick.  Use
         timer_sleep() because it will yield the CPU to other
         processes. */                
      timer_sleep (ticks); 
    }
  else 
    {
      /* Otherwise, use a busy-wait loop for more accurate
         sub-tick timing. */
      real_time_delay (num, denom); 
    }
}

/* Busy-wait for approximately NUM/DENOM seconds. */
static void
real_time_delay (int64_t num, int32_t denom)
{
  /* Scale the numerator and denominator down by 1000 to avoid
     the possibility of overflow. */
  ASSERT (denom % 1000 == 0);
  busy_wait (loops_per_tick * num / 1000 * TIMER_FREQ / (denom / 1000)); 
}
