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
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* Random value for basic thread
   Do not modify this value. */
#define THREAD_BASIC 0xd42df210

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list ready_list;
/* 🚨 alarm clock 추가 : sleep list 구조체 정의 */
static struct list sleep_list;

/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Thread destruction requests */
static struct list destruction_req;

/* Statistics. */
static long long idle_ticks;   /* # of timer ticks spent idle. */
static long long kernel_ticks; /* # of timer ticks in kernel threads. */
static long long user_ticks;   /* # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4		  /* # of timer ticks to give each thread. */
static unsigned thread_ticks; /* # of timer ticks since last yield. */

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static void do_schedule(int status);
static void schedule (void);
static tid_t allocate_tid (void);

/* Returns true if T appears to point to a valid thread. */
#define is_thread(t) ((t) != NULL && (t)->magic == THREAD_MAGIC)

/* Returns the running thread.
 * Read the CPU's stack pointer `rsp', and then round that
 * down to the start of a page.  Since `struct thread' is
 * always at the beginning of a page and the stack pointer is
 * somewhere in the middle, this locates the curent thread. */
#define running_thread() ((struct thread *) (pg_round_down (rrsp ())))


// Global descriptor table for the thread_start.
// Because the gdt will be setup after the thread_init, we should
// setup temporal gdt first.
static uint64_t gdt[3] = { 0, 0x00af9a000000ffff, 0x00cf92000000ffff };

/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. 
   🚨 커널 스레드를 위한 기본 초기화 수행 */
void thread_init(void)
{
	ASSERT(intr_get_level() == INTR_OFF);

	/* Reload the temporal gdt for the kernel
	 * This gdt does not include the user context.
	 * The kernel will rebuild the gdt with user context, in gdt_init (). */
	struct desc_ptr gdt_ds = {
		.size = sizeof (gdt) - 1,
		.address = (uint64_t) gdt
	};
	lgdt (&gdt_ds);

	/* Init the globla thread context */
	lock_init(&tid_lock);
	list_init(&ready_list);
	list_init(&sleep_list); 			/* 🚨 sleep_list 초기화 */
	list_init(&destruction_req);

	/* Set up a thread structure for the running thread. */
	initial_thread = running_thread ();
	init_thread (initial_thread, "main", PRI_DEFAULT);
	initial_thread->status = THREAD_RUNNING;
	initial_thread->tid = allocate_tid ();
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void thread_start(void)
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

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */
void
thread_tick (void) {
	struct thread *t = thread_current ();

	/* Update statistics. */
	if (t == idle_thread)
		idle_ticks++;
#ifdef USERPROG
	else if (t->pml4 != NULL)
		user_ticks++;
#endif
	else
		kernel_ticks++;

	/* Enforce preemption. */
	if (++thread_ticks >= TIME_SLICE)
		intr_yield_on_return ();
}

/* Prints thread statistics. */
void thread_print_stats(void)
{
	printf("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
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
   /* create() : 새로운 스레드를 생성하는 함수 
   	- 입력값 : 생성할 스레드의 이름, 우선순위, 생성할 스레드가 실행할 함수의 포인터, 그 함수에 전달할 인자 */
tid_t thread_create(const char *name, int priority,
					thread_func *function, void *aux)
{
	/* 새로 생성할 구조체와 스레드 id를 저장할 변수 선언 */
	struct thread *t;
	tid_t tid;

	ASSERT(function != NULL);	/* 입력받은 함수 포인터 function이 null이 아닌지 확인 */

	/* Allocate thread. : 새로운 스레드를 할당 */
	t = palloc_get_page(PAL_ZERO);	/* 페이지 할당기에서 새로운 구조체를 할당 */
	if (t == NULL) 					/* 할당 실패 시, TID_ERROR 반환 */
		return TID_ERROR;

	/* Initialize thread. : 스레드 초기화 */
	init_thread(t, name, priority);	/* 할당받은 구조체 초기화 */
	tid = t->tid = allocate_tid();	/* 스레드 id 할당 */

	/* Call the kernel_thread if it scheduled.
	 * Note) rdi is 1st argument, and rsi is 2nd argument. */
	/* 스레드의 레지스터 값을 설정 */
	t->tf.rip = (uintptr_t)kernel_thread;
	t->tf.R.rdi = (uint64_t)function;
	t->tf.R.rsi = (uint64_t)aux;
	t->tf.ds = SEL_KDSEG;
	t->tf.es = SEL_KDSEG;
	t->tf.ss = SEL_KDSEG;
	t->tf.cs = SEL_KCSEG;
	t->tf.eflags = FLAG_IF;

	/* Add to run queue. : ready_list에 추가 */
	thread_unblock(t);
	thread_preemption();

	/* 생성된 스레드의 id 반환 */
	return tid;
}

/* 🚨 alarm clock : sleep 함수 추가 
	- 1. intr_disable()으로 인터럽트를 끄고, 이전 인터럽트 상태를 변수에 저장
	- 2. list_push_back() 함수로 스레드를 sleep_list에 추가하고, 스레드 상태를 block으로 변경
	- 3. intr_set_level()으로 이전 인터럽트 레벨을 복원 */
void thread_sleep(int64_t ticks)
{
	struct thread *cur; 			/* thread를 가리키는 포인터 cur 선언 */
	enum intr_level old_level;
	/* enum intr_level은 인터럽트의 상태를 나타내는 데이터 타입(활성/비활성) */

	old_level = intr_disable(); 	/* 반환된 이전 인터럽트 상태를 old_level에 저장 */
	/* inter_disable() : 인터럽트를 비활성화하고, 이전 인터럽트 상태를 반환하는 함수.
	이전 인터럽트 상태를 저장해두면, 이 후 인터럽트를 다시 활성화할 때 이전 상태로 쉽게 복원이 가능함 */

	cur = thread_current();			/* thread_curret를 통해 현재 실행 중인 스레드를 cur에 저장 */
	ASSERT(cur != idle_thread); 	/* cur이 idle thread가 아님을 검사 */

	/* 현재 쓰레드(cur)의 wakeup 변수에 쓰레드가 일어나야 할 ticks값을 저장 */
	cur->wakeup = ticks;			
	
	/* list_push_back()으로 sleep_list의 맨 뒤에 현재 스레드 추가
		- sleep_list에는 struct thread 구조체를 가리키는 포인터가 저장되어 있음! 
		- &cur->elem : 연결리스트의 상태(prev/next)를 나타내는 list_elem의 구조체의 주소를 가리키는 것*/
	list_push_back(&sleep_list, &cur->elem);
	thread_block();					/* 현재 스레드를 bocked 상태로 변경하고 대기 큐에서 제거 */

	/* intr_set_level 은 현재 인터럽트의 레벨을 바꿔줌.*/
	intr_set_level(old_level);		/* 이전 인터럽트 레벨로 복원! */
}

/* 🚨 alarm clock : awake 함수 추가 */
void thread_awake(int64_t ticks)
{	
	/* 포인터 e가 sleep_list의 시작점을 가리키게 함 */
	struct list_elem *e = list_begin (&sleep_list);
	/* sleep_list()의 끝까지 반복문을 수행 */
  	while (e != list_end (&sleep_list)) {
		/* 포인터 e가 가리키는 구조체를 thread 구조체 포인터 t로 변환 */
		struct thread *t = list_entry (e, struct thread, elem);
		if (t->wakeup <= ticks) {	/* 현재 스레드가 일어날 시간이 되었다면 */
			e = list_remove (e);	/* sleep_list 에서 제거 -> 해당 원소의 앞뒤 원소를 서로 연결시켜 줌 */
			thread_unblock (t);		/* 스레드를 unblock */
		}
		else 
			e = list_next (e);		/* 아직 일어날 시간이 아니면 다음 원소를 검사 */
	}
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void thread_block(void)
{
	ASSERT(!intr_context());
	ASSERT(intr_get_level() == INTR_OFF);
	thread_current()->status = THREAD_BLOCKED; // 스레드 현재 상태를 block으로 바꿔주기
	schedule();								   // 다시 스케줄링
}

/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */
void thread_unblock(struct thread *t)
{
	enum intr_level old_level;

	ASSERT(is_thread(t));

	old_level = intr_disable();
	ASSERT(t->status == THREAD_BLOCKED);
	// list_push_back(&ready_list, &t->elem);		/* 기존 list_push_back 함수 주석 처리 */
	/* 🌸 list_insert_order 함수 추가 
		- ready_list에 스레드 t의 elem 요소를 우선순위 비교 방식으로 추가 */
	list_insert_ordered (&ready_list, &t->elem, thread_compare_priority, 0);		
	
	t->status = THREAD_READY;
	intr_set_level(old_level);
}

/* Returns the name of the running thread. */
const char *
thread_name(void)
{
	return thread_current()->name;
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *
thread_current(void)
{
	struct thread *t = running_thread();

	/* Make sure T is really a thread.
	   If either of these assertions fire, then your thread may
	   have overflowed its stack.  Each thread has less than 4 kB
	   of stack, so a few big automatic arrays or moderate
	   recursion can cause stack overflow. */
	ASSERT(is_thread(t));
	ASSERT(t->status == THREAD_RUNNING);

	return t;
}

/* Returns the running thread's tid. */
tid_t thread_tid(void)
{
	return thread_current()->tid;
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void thread_exit(void)
{
	ASSERT(!intr_context());

#ifdef USERPROG
	process_exit();
#endif

	/* Just set our status to dying and schedule another process.
	   We will be destroyed during the call to schedule_tail(). */
	intr_disable();
	do_schedule(THREAD_DYING);
	NOT_REACHED();
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
void thread_yield(void)
{
	struct thread *curr = thread_current();
	enum intr_level old_level;

	ASSERT(!intr_context());

	old_level = intr_disable();
	if (curr != idle_thread)
		// list_push_back(&ready_list, &curr->elem);		/* 기존 list_push_back 함수 주석 처리 */
		/* 🌸 list_insert_order 함수 추가 
			- ready_list에 스레드 t의 elem 요소를 우선순위 비교 방식으로 추가 
			- 참고 : struct thread 는 각 스레드를 구분하기 위한 struct list_elem을 멤버로 가지고 있음.
					리스트에서는 각 원소가 struct list_elem의 형태로 존재하기 때문에, 아래처럼 curr 스레드의 elem요소를 전달해야 함. */
		list_insert_ordered (&ready_list, &curr->elem, thread_compare_priority, 0);	
	do_schedule(THREAD_READY);
	intr_set_level(old_level);
}

bool thread_compare_donate_priority (const struct list_elem *aa, 
									const struct list_elem *bb, void *aux UNUSED) {
	struct thread *a = list_entry(aa, struct thread, d_elem);
	struct thread *b = list_entry(bb, struct thread, d_elem);

	return a->priority > b->priority;	
}

/* 🌸 donation 리스트에서 스레드를 지워주는 함수 */
void remove_with_lock (struct lock *lock)
{
	struct list_elem *e;		/* donation 리스트를 순회하기 위한 변수 선언 */
	struct thread *cur = thread_current();
	
	for (e = list_begin(&cur->donations); e != list_end(&cur->donations); e = list_next(e)) {
		/* donation 리스트 순회하며 각 스레드 추출 */
		struct thread *t = list_entry(e, struct thread, d_elem);
		if (t->wait_lock == lock)			/* 추출한 스레드가 현재 lock을 기다리고 있다면 */
			list_remove(&t->d_elem);		/* 해당 스레드를 donation 리스트에서 제거 */
	}
}

/* 🌸 현재 스레드와 ready_list의 맨 앞 쓰레드의 우선순위 비교 함수
	- list_entry는 리스트원소 list_elem 이 어떤 구조체의 멤버의 일부분인지를 나타내는 포인터 반환 
	- 즉, ready_list의 가장 앞 요소가 어떤 스레드 구조체에 속해있는 elem요소인지 알아보고 해당 구조체의 포인터 반환 
	- 리스트에서는 각 원소가 struct list_elem의 형태로 존재 */
void thread_preemption(void)
{	
	if (!list_empty(&ready_list) && 
		thread_current()->priority < 
		list_entry(list_front(&ready_list), struct thread, elem)->priority)
		thread_yield();
}

/* 🌸 priority 재설정 함수 */
void refresh_priority(void)
{
	struct thread *cur = thread_current();
	cur->priority = cur->init_priority;		/* 처음 우선순위로 다시 바꿔 줌 */
	
	if (!list_empty(&cur->donations)) {
		list_sort(&cur->donations, thread_compare_donate_priority, 0);
		
		struct thread *front = list_entry(list_front(&cur->donations), struct thread, d_elem);
		if (cur->priority < front->priority)		/* 만약 donation 리스트의 맨 앞 요소의 우선순위가 더 높으면 */
			cur->priority = front->priority;		/* 우선순위를 그걸로 바꿔줘야 함 */
	}
}

/* 🌸 priority donation 함수 
	- 내 우선순위를 lock을 점유하고 있는 스레드에 빌려줌 
	- donation은 여러번 중첩되어 실행될 수 있으므로 깊이에 적당한 제한을 둘 것.(nested 방지) 
	- 내가 대기중인 lock을 가진 스레드가 없을 때까지 lock chain을 따라가면서 우선순위를 기부 */
void donate_priority(void)
{
	int depth;
	struct thread *cur = thread_current();

	for (depth = 0; depth < 8; depth++) {		/* 최대 8번만 반복 */
		if (!cur->wait_lock) 
			break;
		struct thread *holder = cur->wait_lock->holder;
		holder->priority = cur->priority;	/* 현재 스레드의 우선순위 기부 */
		cur = holder;						/* 현재 스레드를 우선순위를 기부받은 스레드로 대체 */
	}
}

/* 🌸 두 스레드의 우선순위 비교 함수 
	- 두개의 리스트 요소 aa, bb를 받아서 해당 요소가 기리키는 스레드 a와 b의 우선순위를 비교
	- list_insert_ordered에서 less 대신 사용 */
bool thread_compare_priority(struct list_elem *aa, struct list_elem *bb, void *aux UNUSED)
{
	struct thread *a = list_entry(aa, struct thread, elem);
	struct thread *b = list_entry(bb, struct thread, elem);
	return a->priority > b->priority;		/* 스레드 a와 b의 우선순위를 비교하여 true/false를 반환 */
}

/* 🌸 Sets the current thread's priority to NEW_PRIORITY. 
	- 현재 스레드의 우선순위를 새 우선순위로 설정. 현재 스레드의 우선순위가 더 이상 높지 않으면 우선순위 양보 */
void thread_set_priority(int new_priority)
{
	thread_current()->init_priority = new_priority;

	refresh_priority();
	thread_preemption();
}

/* 🌸 Returns the current thread's priority. 
	- 현재 스레드의 우선순위를 반환. 우선 순위 기부가 있는 경우, 더 높은 우선 순위를 반환 */
int thread_get_priority(void)
{
	return thread_current()->priority;
}

/* Sets the current thread's nice value to NICE. */
void thread_set_nice(int nice UNUSED)
{
	/* TODO: Your implementation goes here */
}

/* Returns the current thread's nice value. */
int thread_get_nice(void)
{
	/* TODO: Your implementation goes here */
	return 0;
}

/* Returns 100 times the system load average. */
int thread_get_load_avg(void)
{
	/* TODO: Your implementation goes here */
	return 0;
}

/* Returns 100 times the current thread's recent_cpu value. */
int thread_get_recent_cpu(void)
{
	/* TODO: Your implementation goes here */
	return 0;
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
idle(void *idle_started_ UNUSED)
{
	struct semaphore *idle_started = idle_started_;

	idle_thread = thread_current();
	sema_up(idle_started);

	for (;;)
	{
		/* Let someone else run. */
		intr_disable();
		thread_block();

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
kernel_thread (thread_func *function, void *aux) {
	ASSERT (function != NULL);

	intr_enable ();       /* The scheduler runs with interrupts off. */
	function (aux);       /* Execute the thread function. */
	thread_exit ();       /* If function() returns, kill the thread. */
}

/* Does basic initialization of T as a blocked thread named
   NAME. 
   🌸 유저 스레드를 위한 초기화 수행 */
static void
init_thread (struct thread *t, const char *name, int priority) {
	ASSERT (t != NULL);
	ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
	ASSERT (name != NULL);

	memset (t, 0, sizeof *t);
	t->status = THREAD_BLOCKED;
	strlcpy (t->name, name, sizeof t->name);
	t->tf.rsp = (uint64_t) t + PGSIZE - sizeof (void *);
	t->priority = priority;
	t->magic = THREAD_MAGIC;

	/* 🌸 스레드 구조체에 추가한 요소들 추가로 초기화 */
	t->init_priority = priority;
	t->wait_lock = NULL;
	list_init (&t->donations);

}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread *
next_thread_to_run(void)
{
	if (list_empty(&ready_list))
		return idle_thread;
	else
		return list_entry(list_pop_front(&ready_list), struct thread, elem);
}

/* Use iretq to launch the thread */
void do_iret(struct intr_frame *tf)
{
	__asm __volatile(
		"movq %0, %%rsp\n"
		"movq 0(%%rsp),%%r15\n"
		"movq 8(%%rsp),%%r14\n"
		"movq 16(%%rsp),%%r13\n"
		"movq 24(%%rsp),%%r12\n"
		"movq 32(%%rsp),%%r11\n"
		"movq 40(%%rsp),%%r10\n"
		"movq 48(%%rsp),%%r9\n"
		"movq 56(%%rsp),%%r8\n"
		"movq 64(%%rsp),%%rsi\n"
		"movq 72(%%rsp),%%rdi\n"
		"movq 80(%%rsp),%%rbp\n"
		"movq 88(%%rsp),%%rdx\n"
		"movq 96(%%rsp),%%rcx\n"
		"movq 104(%%rsp),%%rbx\n"
		"movq 112(%%rsp),%%rax\n"
		"addq $120,%%rsp\n"
		"movw 8(%%rsp),%%ds\n"
		"movw (%%rsp),%%es\n"
		"addq $32, %%rsp\n"
		"iretq"
		:
		: "g"((uint64_t)tf)
		: "memory");
}

/* Switching the thread by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function. */
static void
thread_launch(struct thread *th)
{
	uint64_t tf_cur = (uint64_t)&running_thread()->tf;
	uint64_t tf = (uint64_t)&th->tf;
	ASSERT(intr_get_level() == INTR_OFF);

	/* The main switching logic.
	 * We first restore the whole execution context into the intr_frame
	 * and then switching to the next thread by calling do_iret.
	 * Note that, we SHOULD NOT use any stack from here
	 * until switching is done. */
	__asm __volatile(
		/* Store registers that will be used. */
		"push %%rax\n"
		"push %%rbx\n"
		"push %%rcx\n"
		/* Fetch input once */
		"movq %0, %%rax\n"
		"movq %1, %%rcx\n"
		"movq %%r15, 0(%%rax)\n"
		"movq %%r14, 8(%%rax)\n"
		"movq %%r13, 16(%%rax)\n"
		"movq %%r12, 24(%%rax)\n"
		"movq %%r11, 32(%%rax)\n"
		"movq %%r10, 40(%%rax)\n"
		"movq %%r9, 48(%%rax)\n"
		"movq %%r8, 56(%%rax)\n"
		"movq %%rsi, 64(%%rax)\n"
		"movq %%rdi, 72(%%rax)\n"
		"movq %%rbp, 80(%%rax)\n"
		"movq %%rdx, 88(%%rax)\n"
		"pop %%rbx\n" // Saved rcx
		"movq %%rbx, 96(%%rax)\n"
		"pop %%rbx\n" // Saved rbx
		"movq %%rbx, 104(%%rax)\n"
		"pop %%rbx\n" // Saved rax
		"movq %%rbx, 112(%%rax)\n"
		"addq $120, %%rax\n"
		"movw %%es, (%%rax)\n"
		"movw %%ds, 8(%%rax)\n"
		"addq $32, %%rax\n"
		"call __next\n" // read the current rip.
		"__next:\n"
		"pop %%rbx\n"
		"addq $(out_iret -  __next), %%rbx\n"
		"movq %%rbx, 0(%%rax)\n" // rip
		"movw %%cs, 8(%%rax)\n"	 // cs
		"pushfq\n"
		"popq %%rbx\n"
		"mov %%rbx, 16(%%rax)\n" // eflags
		"mov %%rsp, 24(%%rax)\n" // rsp
		"movw %%ss, 32(%%rax)\n"
		"mov %%rcx, %%rdi\n"
		"call do_iret\n"
		"out_iret:\n"
		:
		: "g"(tf_cur), "g"(tf)
		: "memory");
}

/* Schedules a new process. At entry, interrupts must be off.
 * This function modify current thread's status to status and then
 * finds another thread to run and switches to it.
 * It's not safe to call printf() in the schedule(). */
static void
do_schedule(int status)
{
	ASSERT(intr_get_level() == INTR_OFF);
	ASSERT(thread_current()->status == THREAD_RUNNING);
	while (!list_empty(&destruction_req))
	{
		struct thread *victim =
			list_entry(list_pop_front(&destruction_req), struct thread, elem);
		palloc_free_page(victim);
	}
	thread_current()->status = status;
	schedule();
}

static void
schedule(void)
{
	struct thread *curr = running_thread();			/* curr : 현재 실행중인 스레드 */
	struct thread *next = next_thread_to_run();		/* next : 다음으로 CPU 점유권을 넘겨받는 스레드 */
	/* 다음 cpu 점유권을 넘겨받는 스레드는 next_thread_to_run()을 통해 결정됨 
	next_thread_to_run()에서는 list_pop_front(&ready_list)를 통해 단순히 가장 앞의 스레드를 반환함 
	즉, 현재 FIFO 상태로 되어 있으며, 제대로 된 우선순위 스케줄링이 되어 있지 않음 */

	ASSERT(intr_get_level() == INTR_OFF);
	ASSERT(curr->status != THREAD_RUNNING);
	ASSERT(is_thread(next));
	/* Mark us as running. */
	next->status = THREAD_RUNNING;

	/* Start new time slice. */
	thread_ticks = 0;

#ifdef USERPROG
	/* Activate the new address space. */
	process_activate(next);
#endif

	if (curr != next)
	{
		/* If the thread we switched from is dying, destroy its struct
		   thread. This must happen late so that thread_exit() doesn't
		   pull out the rug under itself.
		   We just queuing the page free reqeust here because the page is
		   currently used by the stack.
		   The real destruction logic will be called at the beginning of the
		   schedule(). */
		if (curr && curr->status == THREAD_DYING && curr != initial_thread)
		{
			ASSERT(curr != next);
			list_push_back(&destruction_req, &curr->elem);
		}

		/* Before switching the thread, we first save the information
		 * of current running. */
		thread_launch(next);
	}
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid(void)
{
	static tid_t next_tid = 1;
	tid_t tid;

	lock_acquire(&tid_lock);
	tid = next_tid++;
	lock_release(&tid_lock);

	return tid;
}
