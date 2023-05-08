#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stdint.h>
#include "threads/interrupt.h"
#include "threads/synch.h"
#ifdef VM
#include "vm/vm.h"
#endif


/* 스레드의 상태 정보 : States in a thread's life cycle. */
enum thread_status {
	THREAD_RUNNING,     /* Running thread. */
	THREAD_READY,       /* Not running but ready to run. */
	THREAD_BLOCKED,     /* Waiting for an event to trigger. */
	THREAD_DYING        /* About to be destroyed. */
};

/* Thread identifier type.
   You can redefine this to whatever type you like. */
typedef int tid_t;
#define TID_ERROR ((tid_t) -1)          /* Error value for tid_t. */

/* Thread priorities. */
#define PRI_MIN 0                       /* Lowest priority. */
#define PRI_DEFAULT 31                  /* Default priority. */
#define PRI_MAX 63                      /* Highest priority. */

/*----Project 2 추가 -----*/
#define FD_NUM_LIMIT FDT_PAGES *(1<<10)
#define FDT_PAGES 3

/* A kernel thread or user process.
 *
 * Each thread structure is stored in its own 4 kB page.  The
 * thread structure itself sits at the very bottom of the page
 * (at offset 0).  The rest of the page is reserved for the
 * thread's kernel stack, which grows downward from the top of
 * the page (at offset 4 kB).  Here's an illustration:
 *
 *      4 kB +---------------------------------+
 *           |          kernel stack           |
 *           |                |                |
 *           |                |                |
 *           |                V                |
 *           |         grows downward          |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           +---------------------------------+
 *           |              magic              |
 *           |            intr_frame           |
 *           |                :                |
 *           |                :                |
 *           |               name              |
 *           |              status             |
 *      0 kB +---------------------------------+
 *
 * The upshot of this is twofold:
 *
 *    1. First, `struct thread' must not be allowed to grow too
 *       big.  If it does, then there will not be enough room for
 *       the kernel stack.  Our base `struct thread' is only a
 *       few bytes in size.  It probably should stay well under 1
 *       kB.
 *
 *    2. Second, kernel stacks must not be allowed to grow too
 *       large.  If a stack overflows, it will corrupt the thread
 *       state.  Thus, kernel functions should not allocate large
 *       structures or arrays as non-static local variables.  Use
 *       dynamic allocation with malloc() or palloc_get_page()
 *       instead.
 *
 * The first symptom of either of these problems will probably be
 * an assertion failure in thread_current(), which checks that
 * the `magic' member of the running thread's `struct thread' is
 * set to THREAD_MAGIC.  Stack overflow will normally change this
 * value, triggering the assertion. */
/* The `elem' member has a dual purpose.  It can be an element in
 * the run queue (thread.c), or it can be an element in a
 * semaphore wait list (synch.c).  It can be used these two ways
 * only because they are mutually exclusive: only a thread in the
 * ready state is on the run queue, whereas only a thread in the
 * blocked state is on a semaphore wait list. */

/* 각 스레드의 정보를 담는 구조체 */
struct thread {
	/* Owned by thread.c. */
	tid_t tid;                          /* Thread identifier. */
	enum thread_status status;          /* Thread state. */
	char name[16];                      /* Name (for debugging purposes). */
	int priority;                       /* Priority. */

	/* 🚨 alarm clock 추가 */
	int64_t wakeup;						/* 일어나야 하는 ticks 값 */

	/* Shared between thread.c and synch.c. */
	/* 스레드 구조체가 연결리스트에서 사용될 때 필요한 정보를 담고 있음. 
		- prev와 next라는 두 개의 포인터 변수를 가지고 있으며, 각각 리스트의 이전과 다음 원소를 가리킴 */
	struct list_elem elem;              /* List element. */

	/* 🌸 스레드 priority donation 관련 항목 추가 
		- multiple donation을 해결하기 위해 우선순위를 나눠준 스레드들을 리스트로 만들어 따로 관리! */
	int init_priority; 					/* 우선순위를 양도받을 때, 원래의 우선순위를 저장할 변수 */
	struct lock *wait_lock;				/* 해당 스레드가 얻기 위해 기다리고 있는 lock */
	struct list donations;				/* 해당 스레드에게 우선순위를 기부한 스레드들의 리스트 */
	struct list_elem d_elem;			/* donations 리스트를 관리하기 위한 element */

	/*----- project 2 : syscall 추가 ------*/
	int exit_status;					/* 프로세스의 종료 상태 */
	struct semaphore wait_sema;

	struct list child_list;
	struct list_elem child_elem;
	struct intr_frame parent_if;

	struct semaphore fork_sema;
	struct semaphore free_sema;

	struct file **file_descriptor_table;	/* 파일 디스크립터 테이블 & 인덱스 */
	int fd_idx;
	int stdin_count;
	int stdout_count;
	
	struct file *running;

#ifdef USERPROG
	/* Owned by userprog/process.c. */
	uint64_t *pml4;                     /* Page map level 4 */
#endif
#ifdef VM
	/* Table for whole virtual memory owned by thread. */
	struct supplemental_page_table spt;
#endif

	/* Owned by thread.c. */
	struct intr_frame tf;               /* Information for switching */
	unsigned magic;                     /* Detects stack overflow. */
};

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
extern bool thread_mlfqs;

void thread_init (void);
void thread_start (void);

void thread_tick (void);
void thread_print_stats (void);

typedef void thread_func (void *aux);
tid_t thread_create (const char *name, int priority, thread_func *, void *);

/* 🚨 alarm clock 관련 함수원형 선언 */
void thread_sleep(int64_t ticks);
void thread_awake(int64_t ticks);

void thread_block (void);
void thread_unblock (struct thread *);

struct thread *thread_current (void);
tid_t thread_tid (void);
const char *thread_name (void);

void thread_exit (void) NO_RETURN;
void thread_yield (void);

/* 🌸 쓰레드 우선순위 비교 함수 원형 선언 */
bool thread_compare_donate_priority (const struct list_elem *l, const struct list_elem *s, void *aux UNUSED);
void remove_with_lock (struct lock *lock);
void thread_preemption(void);
void refresh_priority(void);
void donate_priority(void);
bool sema_compare_priority(const struct list_elem *aa, const struct list_elem *bb, void *aux);
bool thread_compare_priority(struct list_elem *aa, struct list_elem *bb, void *aux UNUSED);
int thread_get_priority (void);
void thread_set_priority (int);

int thread_get_nice (void);
void thread_set_nice (int);
int thread_get_recent_cpu (void);
int thread_get_load_avg (void);

void do_iret (struct intr_frame *tf);

/* project 2 cnrk */
struct thread *get_child(int pid);

#endif /* threads/thread.h */
