#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
/* 추가! */
#include "threads/synch.h"
#include "userprog/syscall.h"
#ifdef VM
#include "vm/vm.h"
#endif

static void process_cleanup (void);
static bool load (const char *file_name, struct intr_frame *if_);
static void initd (void *f_name);
static void __do_fork (void *);

/* 추가 */
void argument_stack(struct intr_frame *if_, char **argv, int argc);

/* General process initializer for initd and other process. */
static void
process_init (void) {
	struct thread *current = thread_current ();
}

/* Starts the first userland program, called "initd", loaded from FILE_NAME.
 * The new thread may be scheduled (and may even exit)
 * before process_create_initd() returns. Returns the initd's
 * thread id, or TID_ERROR if the thread cannot be created.
 * Notice that THIS SHOULD BE CALLED ONCE. */
tid_t
process_create_initd (const char *file_name) {		/* file_name : 실행파일 이름 */
	char *fn_copy;									/* 파일 이름을 복사할 버퍼 */
	tid_t tid;										/* 스레드 id값 */

	/* Make a copy of FILE_NAME.
	 * Otherwise there's a race between the caller and load(). */
	fn_copy = palloc_get_page (0);					/* palloc 함수로 페이지 할당 */
	/* void *palloc_get_page (enum palloc_flags);
	 * 여기에 인자로 0을 전달하면 기본적인 페이지 할당을 수행하고, 할당된 페이지를 0으로 초기화! */
	if (fn_copy == NULL)							/* 할당 실패시 에러값 반환 */
		return TID_ERROR;
	strlcpy (fn_copy, file_name, PGSIZE);			/* 실행파일 이름을 fn_copy로 복사 */
	          										/* 실행파일 이름이 복사본을 만들어서 이름이 변경되는 경우를 방지 */

	/* project 2 추가 */
	// char *save_ptr;
	// file_name = strtok_r(file_name, " ", &save_ptr);
	/* Create a new thread to execute FILE_NAME. */
	tid = thread_create (file_name, PRI_DEFAULT, initd, fn_copy);	/* initd 함수를 실행하는 새로운 스레드 생성 */
	if (tid == TID_ERROR)							/* 스레드 생성 실패시 할당된 페이지 해제 & 에러값 반환 */
		palloc_free_page (fn_copy);
	return tid;										/* 생성된 스레드 id값 반환 */
}

/* A thread function that launches first user process. */
static void
initd (void *f_name) {								/* f_name : 실행파일 이름이 저장된 포인터 */
#ifdef VM
	/* Pintos에서는 가상메모리 관리를 위해 스레드마다 보조 페이지 테이블을 사용 
	 * 만약 가상메모리(VM) 기능이 활성화되어 있다면, 현재 스레드의 보조 페이지 테이블을 초기화 */
	supplemental_page_table_init (&thread_current ()->spt);
#endif

	process_init ();								/* 프로세스 초기화 */

	if (process_exec (f_name) < 0)					/* process_exec으로 f_name 실행 */
		PANIC("Fail to launch initd\n");			/* 실행 실패시, 에러 메세지 출력 & 커널을 패닉 상태로 만듦 */
	NOT_REACHED ();
	/* NOT_REACHED : 커널을 패닉 상태로 만드는 함수
	 * 코드의 논리적 흐름상 실행될 수 없는 코드이나, 만약을 대비해 작성된 부분 
	 * if문이 ture인 경우, exec함수가 리턴하지 않고 패닉상태로 종료됨. 이 경우를 대비해 작성됨 
	 * if문이 false인 경우, exec함수가 리턴값을 반환하면서 not_reached함수는 실행되지 않음 */
}

/* Clones the current process as `name`. Returns the new process's thread id, or
 * TID_ERROR if the thread cannot be created. 
 * 🌸 fork 함수는 부모 프로세스에서 자식 프로세스를 생성하는 함수!
 * 자식 프로세스의 pid를 반환하고, 자식 프로세스에서 리턴값은 0이어야 함.
 * 부모 프로세스는 자식이 성공적으로 복제된 것을 확인한 뒤에 fork로부터 리턴!
 * 복제 실패 시 tid_error 반환! */
tid_t
process_fork (const char *name, struct intr_frame *if_ UNUSED) {
	/* Clone current thread to new thread.*/
	/* 1. 자식 프로세스 생성 전, 현재 스레드의 인터럽트 프레임을 복사! 
	 * if_가 가리키는 인터럽트 프레임을 cur의 parent_if에 intr_fram 바이트 수만큼 복사 */
	struct thread *cur = thread_current();
	memcpy(&cur->parent_if, if_, sizeof(struct intr_frame));

	/* 2. 새로운 스레드 생성!
	 * thread_create는 스레드를 생성하고 tid를 반환.(오류 발생 시 tid_error) */
	tid_t pid = thread_create (name, PRI_DEFAULT, __do_fork, cur);
	if (pid == TID_ERROR) 
		return TID_ERROR;

	/* 3. 새로운 스레드의 세마포어를 내리고, 자식프로세스가 로딩될 때까지 대기!
	 * 반환된 tid에 해당하는 스레드 찾아서 child에 저장 
	 * 부모 스레드는 자식 스레드를 미리 저장해놓고, 해당 정보를 확인하며 오류 여부 등을 확인할 수 있음 */
	struct thread *child = get_child(pid);		/*????? 이 함수 어디감? 내가 만들어야 되는구나....^^...*/
	sema_down(&child->fork_sema);
	if (child->exit_status == -1)
		return TID_ERROR;

	return pid;
}

#ifndef VM
/* Duplicate the parent's address space by passing this function to the
 * pml4_for_each. This is only for the project 2. */
/* 페이지 테이블 엔트리(PTE) 복제 함수 */
static bool
duplicate_pte (uint64_t *pte, void *va, void *aux) {
	struct thread *current = thread_current ();
	struct thread *parent = (struct thread *) aux;
	void *parent_page;
	void *newpage;
	bool writable;

	/* 1. TODO: If the parent_page is kernel page, then return immediately. */
	/* 현재 가상주소(va)가 커널 주소인 경우 무시 */
	if(is_kernel_vaddr(va))
		return true;
	/* 2. Resolve VA from the parent's page map level 4. */
	/* 부모 페이지 테이블에서 va 해당하는 가상주소의 PTE를 가져와서 parent_page에 저장 */
	parent_page = pml4_get_page (parent->pml4, va);
	if (parent_page == NULL)
		return false;

	/* 3. TODO: Allocate new PAL_USER page for the child and set result to
	 *    TODO: NEWPAGE. */
	/* 자식 스레드의 페이지 테이블에 새로운 페이지를 할당하고, 그 주소를 newpage에 저장 
	 * 할당된 페이지는 PAL_USER 옵션을 가지며, 유저 프로그램이 접근 가능한 공간에 위치 */
	newpage = palloc_get_page(PAL_USER);
	if(newpage == NULL)
		return false;
	/* 4. TODO: Duplicate parent's page to the new page and
	 *    TODO: check whether parent's page is writable or not (set WRITABLE
	 *    TODO: according to the result). */
	/* 부모 페이지를 새로운 페이지에 복제*/
	memcpy(newpage, parent_page, PGSIZE);
	writable = is_writable(pte);		/* pte 읽고 쓰기가 가능한지 확인 */
	/* 5. Add new page to child's page table at address VA with WRITABLE
	 *    permission. */
	if (!pml4_set_page (current->pml4, va, newpage, writable)) {
		/* 6. TODO: if fail to insert page, do error handling. */
		/* 페이지 추가에 실패한 경우, 오류 처리*/
		return false;
	}
	return true;
}
#endif

/* A thread function that copies parent's execution context.
 * Hint) parent->tf does not hold the userland context of the process.
 *       That is, you are required to pass second argument of process_fork to
 *       this function. */
/* 부모의 CPU 컨텍스트 복사, 자식 주소공간 초기화 및 시작 함수 */
static void
__do_fork (void *aux) {
	struct intr_frame if_;
	struct thread *parent = (struct thread *) aux;	/* 인자로 받은 aux 스레드를 parent에 저장 */
	struct thread *current = thread_current ();		/* 현재 스레드를 current에 저장*/
	/* TODO: somehow pass the parent_if. (i.e. process_fork()'s if_) */
	struct intr_frame *parent_if;
	bool succ = true;

	/* Read the cpu context to local stack. */
	/* 1. aux로 전달받은 parent 스레드의 컨텍스트를 로컬 스택에 복사! */
	parent_if = &parent->parent_if;
	memcpy (&if_, parent_if, sizeof (struct intr_frame));
	if_.R.rax = 0;

	/* Duplicate PT 
	 * 2. 새로운 스레드의 페이지 디렉토리 생성 */
	current->pml4 = pml4_create();		/* 현재 스레드의 페이지 디렉토리 생성 */
	if (current->pml4 == NULL)			/* 디렉토리 생성 실패 시, error로 점프 */
		goto error;

	process_activate (current);
#ifdef VM
	supplemental_page_table_init (&current->spt);
	if (!supplemental_page_table_copy (&current->spt, &parent->spt))
		goto error;
#else
	/* 3. 부모의 페이지 테이블을 복사. 실패 시 에러 반환 */
	if (!pml4_for_each (parent->pml4, duplicate_pte, parent))
		goto error;
#endif

	/* TODO: Your code goes here.
	 * TODO: Hint) To duplicate the file object, use `file_duplicate`
	 * TODO:       in include/filesys/file.h. Note that parent should not return
	 * TODO:       from the fork() until this function successfully duplicates
	 * TODO:       the resources of parent.*/
	
	/* fd 테이블에 복사할 데이터가 일정 갯수 이상이면 에러 발생! */
	if (parent->fd_idx >= FD_NUM_LIMIT)
		goto error;

	/* 기본 fd 복사. 2부터 시작. 3부터 해야 하는거 아닐지...*/
	current->file_descriptor_table[0] = parent->file_descriptor_table[0];
	current->file_descriptor_table[1] = parent->file_descriptor_table[1];
	int fd = 2;
	struct file *f;

	current->fd_idx = parent->fd_idx;

	/* 5. 부모의 파일 디스크립터 테이블에서 열린 파일을 복사 */
	for (fd; fd < FD_NUM_LIMIT; fd++) {
		f = parent->file_descriptor_table[fd];
		if (f == NULL)
			continue;
		current->file_descriptor_table[fd] = file_duplicate(f);
	}

	current->fd_idx = parent->fd_idx;
	/* 세마포어값을 증가시켜서 부모 프로세스에게 자식이 생성되었음을 알림 */
	sema_up(&current->fork_sema);
	// process_init();
	
	/* Finally, switch to the newly created process. */
	if (succ)
		do_iret (&if_);
error:
	current->exit_status = TID_ERROR;
	sema_up(&current->fork_sema);
	exit(TID_ERROR);
	// thread_exit ();
}

/* Switch the current execution context to the f_name.
 * Returns -1 on fail. */
int
process_exec (void *f_name) {
	char *file_name = f_name;				/* void로 넘겨받았기 때문에 문자열 인식을 위해 char로 바꿔줌 */
	bool success;							/* 바이너리 파일의 로드 성공 여부 저장 */		

/* project 2 : 커맨드 라인 파싱 */
	char *argv[128];						/* 커맨드 라인 문자열 배열은 최대 128바이트 */
	char *token, *save_ptr;
	int argc = 0;							/* argc : 현재까지 저장된 인자의 갯수*/
	
	token = strtok_r(file_name, " ", &save_ptr);
	while(token != NULL) {
		argv[argc] = token;
		token = strtok_r(NULL, " ", &save_ptr);
		argc++;
	}

	/* We cannot use the intr_frame in the thread structure.
	 * This is because when current thread rescheduled,
	 * it stores the execution information to the member. */
	struct intr_frame _if;				  /* _if 구조체 : 인터럽트 발생 시 실행되는 코드의 정보 저장 */
	_if.ds = _if.es = _if.ss = SEL_UDSEG; /* 데이터, 코드, 스택 세그먼트(ds, es, ss, cs)를 유저 데이터 세그먼트(SEL_UDSEG)로 설정 */
	_if.cs = SEL_UCSEG;					  /* 유저모드에서 실행되는 프로세스가 접근할 수 있는 메모리 영역을 설정하는 작업 */
	_if.eflags = FLAG_IF | FLAG_MBS;

	/* We first kill the current context */
	process_cleanup ();						/* 현재 프로세스에 할당된 페이지 디렉토리를 지움 */

	/* And then load the binary */
	success = load (file_name, &_if);		/* load 함수 호출 & 실행 결과를 success에 저장*/

	/* If load failed, quit. */
	// palloc_free_page (file_name);			/* 포인터 메모리 해제 */
	if (!success)							/* load 함수 실패 시, -1을 리턴하고 종료 */
	{
		return -1;
	}	

		/* 유저 스택에 인자 저장 */
	argument_stack(&_if, argv, argc);

	hex_dump(_if.rsp, _if.rsp, USER_STACK-_if.rsp, true);
	
	// palloc_free_page(file_name);

	/* Start switched process. */
	do_iret (&_if);							/* _if 구조체에 저장된 새로운 프로그램의 시작점으로 이동 */
	NOT_REACHED ();
}

/* 유저 스택에 프로그램명, 함수인자 저장 
 * parse : 메모리공간, count : 인자 갯수, esp : 스택 포인터 주소값 
 * 64비트 환경에서 포인터는 8바이트이므로, 스택포인터를 8의 배수로 맞춰주어야 한다. */
void 
argument_stack(struct intr_frame *if_, char **argv, int argc) {
	char *argv_address[128];						/* 각 인자가 저장될 스택 주소를 저장하기 위한 배열 */
	int i;

	/* 각 인자를 유저스택에 저장 */
	for (i = argc - 1; i >= 0; i--) {			/* argc-1부터 0까지 역순으로 인덱스를 탐색 */
		if (argv[i] == NULL) {
			continue;
		}
		size_t len = strlen(argv[i]);			/* 각 인자의 길이를 계산 */								
		if_->rsp = if_->rsp - (len+1);
		memcpy((if_->rsp), argv[i], len+1);		/* memcpy함수로 각 인자를 해당하는 스택 주소에 저장 */
		argv_address[i] = if_->rsp;		/* 각 인자가 저장될 스택 주소를 argv_addr에 저장 */
	}

	/* 스택 포인터를 8의 배수로 정렬 
	 * 8의 배수가 아닐 경우, 0으로 채워진 바이트를 스택에 푸시해 정렬을 맞춤 */
	while ((if_->rsp) % 8 != 0) {
		if_->rsp--;
		memset(if_->rsp, 0, sizeof(uint8_t));
		// *(uint8_t *) (if_->rsp) = 0;
	}

	/* 유저 스택에 argv_address 배열의 주소 값들을 푸쉬 */
    for (i = argc; i >= 0; i--) {
		if_->rsp = if_->rsp - 8;
		/* 현재 i와 argc가 같은지 확인 : 같다면 마지막 인자라는 뜻이므로, null pointer sentinel을 푸쉬 */
        if (i == argc) {
            memset(if_->rsp, 0, sizeof(char **));
        } else {	
            memcpy(if_->rsp, &argv_address[i], sizeof(char **));
			/* argv_address[i] 주소값을 그대로 스택에 푸쉬 */
        }
    }

    /* fake return address 추가 */
	if_->rsp = if_->rsp -8;
    memset(if_->rsp, 0, sizeof(void *));

    /* 레지스터 값 설정 */
    if_->R.rdi = argc;					/* rdi에 argc값 전달 */
    if_->R.rsi = if_->rsp + 8;			/* rsi에 argv의 첫번째 원소 주소값 설정 */
}

/* Waits for thread TID to die and returns its exit status.  If
 * it was terminated by the kernel (i.e. killed due to an
 * exception), returns -1.  If TID is invalid or if it was not a
 * child of the calling process, or if process_wait() has already
 * been successfully called for the given TID, returns -1
 * immediately, without waiting.
 *
 * This function will be implemented in problem 2-2.  For now, it
 * does nothing. */
int
process_wait (tid_t child_tid UNUSED) {
	/* XXX: Hint) The pintos exit if process_wait (initd), we recommend you
	 * XXX:       to add infinite loop here before
	 * XXX:       implementing the process_wait. */
	
	/* 자식 프로세스 가져옴, 없으면 에러
	 * 자식 종료까지 대기
	  * 자식의 상태정보 가져옴 exitstatus
	  * 자식 프로세스 자원 해제
	  * 상태정보 리턴 */
	/* 주어진 tid로 해당하는 자식 프로세스를 가져옴 */
	struct thread *child = get_child(child_tid);
	if (child == NULL)
		return -1;	
	
	sema_down(&child->wait_sema);

	int exit_status = child->exit_status;
	list_remove(&child->child_elem);

	sema_up(&child->free_sema);

	return exit_status;
}

/* Exit the process. This function is called by thread_exit (). */
void
process_exit (void) {
	struct thread *cur = thread_current ();
	
	/* TODO: Your code goes here.
	 * TODO: Implement process termination message (see
	 * TODO: project2/process_termination.html).
	 * TODO: We recommend you to implement process resource cleanup here. */

	/* 프로세스에 열린 모든 파일 닫기 */
	for (int fd = 0; fd <= FD_NUM_LIMIT; fd++) {
		close(fd);
	}
	/* 파일 디스크립터 테이블 메모리 해제 */
	palloc_free_multiple(cur->file_descriptor_table, FDT_PAGES);
	file_close(cur->running);

	/* 부모 프로세스 깨우기 */
	sema_up(&cur->wait_sema);
	/* 자식 프로세스가 완전히 종료되기 전에 자식의 자원이 해제되지 않도록 지연시켜 줌
	 * 자식의 자원이 모두 안전하게 해제될 수 있게 함. */
	sema_down(&cur->free_sema);

	process_cleanup ();
}

/* 특정 tid를 가진 스레드를 찾아서 반환해주는 함수 */
struct thread *get_child(int pid) {
	struct thread *cur = thread_current();
	struct list *child_list = &cur->child_list;
	for (struct list_elem *e = list_begin(child_list); e != list_end(child_list); e = list_next(e)) {
		struct thread *t = list_entry(e, struct thread, child_elem);
		if (t->tid == pid)
			return t;
	}
	return NULL;
}

/* Free the current process's resources. */
static void
process_cleanup (void) {
	struct thread *curr = thread_current ();

#ifdef VM
	supplemental_page_table_kill (&curr->spt);
#endif

	uint64_t *pml4;
	/* Destroy the current process's page directory and switch back
	 * to the kernel-only page directory. */
	pml4 = curr->pml4;
	if (pml4 != NULL) {
		/* Correct ordering here is crucial.  We must set
		 * cur->pagedir to NULL before switching page directories,
		 * so that a timer interrupt can't switch back to the
		 * process page directory.  We must activate the base page
		 * directory before destroying the process's page
		 * directory, or our active page directory will be one
		 * that's been freed (and cleared). */
		curr->pml4 = NULL;
		pml4_activate (NULL);
		pml4_destroy (pml4);
	}
}

/* Sets up the CPU for running user code in the nest thread.
 * This function is called on every context switch. */
void
process_activate (struct thread *next) {
	/* Activate thread's page tables. */
	pml4_activate (next->pml4);

	/* Set thread's kernel stack for use in processing interrupts. */
	tss_update (next);
}

/* We load ELF binaries.  The following definitions are taken
 * from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
#define EI_NIDENT 16

#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
 * This appears at the very beginning of an ELF binary. */
struct ELF64_hdr {
	unsigned char e_ident[EI_NIDENT];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint64_t e_entry;
	uint64_t e_phoff;
	uint64_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
};

struct ELF64_PHDR {
	uint32_t p_type;
	uint32_t p_flags;
	uint64_t p_offset;
	uint64_t p_vaddr;
	uint64_t p_paddr;
	uint64_t p_filesz;
	uint64_t p_memsz;
	uint64_t p_align;
};

/* Abbreviations */
#define ELF ELF64_hdr
#define Phdr ELF64_PHDR

static bool setup_stack (struct intr_frame *if_);
static bool validate_segment (const struct Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes,
		bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
 * Stores the executable's entry point into *RIP
 * and its initial stack pointer into *RSP.
 * Returns true if successful, false otherwise. */
/* 🚨 load 함수 파악!!!
 * 1. 주어진 프로그램 파일을 읽고, 페이지 디렉토리 생성 및 활성화 함.
 * 2. 프로그램 헤더를 읽고, 유효성 검사 수행한 뒤, (유효한)세그먼트를 로드함 
 * 3. 유저 스택을 설정하고, 실행파일이 처음 실행할 위치인 엔트리 함수를 설정함 
 * 4. 유저 스택에 커맨드 라인으로 전달된 인자들을 저장 */
static bool
load (const char *file_name, struct intr_frame *if_) {
	struct thread *t = thread_current ();		/* 현재 스레드 저장 */
	struct ELF ehdr;							/* ELF(바이너리) 파일 헤더 정보 저장 구조체 */
	struct file *file = NULL;					/* 프로그램 파일 구조체 포인터. null로 초기화 */
	off_t file_ofs;
	bool success = false;
	int i;

	/* Allocate and activate page directory. 
	 * 현재 스레드의 페이지 디렉토리를 할당하고 활성화 */
	t->pml4 = pml4_create ();					/* pml4_create : 페이지 디렉토리 생성 */
	if (t->pml4 == NULL)
		goto done;
	process_activate (thread_current ());		/* 현재 스레드의 페이지 테이블 활성화 */

	/* Open executable file. 
	 * 주어진 파일이름으로 파일 시스템으로부터 오픈함 */
	file = filesys_open (file_name);			/* 파일 시스템에서 파일을 열고, 해당 파일 구조체를 반환 */
	if (file == NULL) {
		printf ("load: %s: open failed\n", file_name);
		goto done;
	}

	/* 실행 중인 스레드 t의 running 값을 실행할 파일로 초기화 */
	t->running = file;
	file_deny_write(file);

	/* Read and verify executable header. 
	 * 오픈한 실행 파일에서 ELF 헤더 정보를 읽어오고, 적절한 ELF 형식인지 확인함
	 * ELF 형식이 아니거나, 지원하지 않는 형식이거나, 프로그램 헤더가 많을 경우 오류 처리 */
	if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
			|| memcmp (ehdr.e_ident, "\177ELF\2\1\1", 7)
			|| ehdr.e_type != 2
			|| ehdr.e_machine != 0x3E // amd64
			|| ehdr.e_version != 1
			|| ehdr.e_phentsize != sizeof (struct Phdr)
			|| ehdr.e_phnum > 1024) {
		printf ("load: %s: error loading executable\n", file_name);
		goto done;
	}

	/* Read program headers. 
	 * ELF 파일 헤더에서 프로그램 헤더의 위치 정보를 읽어온 후,
	 * 각각의 프로그램 헤더를 순회하면서 해당 메모리 세그먼트를 로드함 */
	file_ofs = ehdr.e_phoff;				/* 프로그램 헤더의 오프셋 값을 변수에 저장 */
	for (i = 0; i < ehdr.e_phnum; i++) {	/* 프로그램 헤더의 갯수만큼 반복 */
		struct Phdr phdr;

		/* file_ofs 값이 음수이거나 파일의 길이보다 크면 done으로 점프 */
		if (file_ofs < 0 || file_ofs > file_length (file))
			goto done;
		file_seek (file, file_ofs);			/* 파일에서 file_ofs 위치로 이동 */

		if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
			goto done;
		file_ofs += sizeof phdr;
		switch (phdr.p_type) {				/* 현재 프로그램 헤더의 타입을 확인 */
			case PT_NULL:
			case PT_NOTE:
			case PT_PHDR:
			case PT_STACK:
			default:
				/* Ignore this segment. */
				break;
			case PT_DYNAMIC:
			case PT_INTERP:
			case PT_SHLIB:
				goto done;
			case PT_LOAD:
				/* 프로그램 헤더에서 읽어온 세그먼트가 유효한지 확인 */
				if (validate_segment (&phdr, file)) {				
					bool writable = (phdr.p_flags & PF_W) != 0;
					uint64_t file_page = phdr.p_offset & ~PGMASK;
					uint64_t mem_page = phdr.p_vaddr & ~PGMASK;
					uint64_t page_offset = phdr.p_vaddr & PGMASK;
					uint32_t read_bytes, zero_bytes;
					if (phdr.p_filesz > 0) {
						/* Normal segment.
						 * Read initial part from disk and zero the rest. */
						read_bytes = page_offset + phdr.p_filesz;
						zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
								- read_bytes);
					} else {
						/* Entirely zero.
						 * Don't read anything from disk. */
						read_bytes = 0;
						zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
					}
					/* 세그먼트 로드 & 실패 시 오류 처리 */
					if (!load_segment (file, file_page, (void *) mem_page,	
								read_bytes, zero_bytes, writable))
						goto done;
				}
				else
					goto done;
				break;
		}
	}

	/* Set up stack : 유저 스택을 준비 */
	if (!setup_stack (if_))
		goto done;

	/* Start address : 실행파일이 가장 처음 실행할 위치, 엔트리 함수 지정 */
	if_->rip = ehdr.e_entry;

	success = true;

done:
	/* We arrive here whether the load is successful or not. 
	 * 파일을 닫고, load의 성공 여부에 관계 없이 공통적으로 done으로 이동함 */
	// file_close (file);
	return success;
}

/* Checks whether PHDR describes a valid, loadable segment in
 * FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Phdr *phdr, struct file *file) {
	/* p_offset and p_vaddr must have the same page offset. */
	if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
		return false;

	/* p_offset must point within FILE. */
	if (phdr->p_offset > (uint64_t) file_length (file))
		return false;

	/* p_memsz must be at least as big as p_filesz. */
	if (phdr->p_memsz < phdr->p_filesz)
		return false;

	/* The segment must not be empty. */
	if (phdr->p_memsz == 0)
		return false;

	/* The virtual memory region must both start and end within the
	   user address space range. */
	if (!is_user_vaddr ((void *) phdr->p_vaddr))
		return false;
	if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
		return false;

	/* The region cannot "wrap around" across the kernel virtual
	   address space. */
	if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
		return false;

	/* Disallow mapping page 0.
	   Not only is it a bad idea to map page 0, but if we allowed
	   it then user code that passed a null pointer to system calls
	   could quite likely panic the kernel by way of null pointer
	   assertions in memcpy(), etc. */
	if (phdr->p_vaddr < PGSIZE)
		return false;

	/* It's okay. */
	return true;
}

#ifndef VM
/* Codes of this block will be ONLY USED DURING project 2.
 * If you want to implement the function for whole project 2, implement it
 * outside of #ifndef macro. */

/* load() helpers. */
static bool install_page (void *upage, void *kpage, bool writable);

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	file_seek (file, ofs);
	while (read_bytes > 0 || zero_bytes > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* Get a page of memory. */
		uint8_t *kpage = palloc_get_page (PAL_USER);
		if (kpage == NULL)
			return false;

		/* Load this page. */
		if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes) {
			palloc_free_page (kpage);
			return false;
		}
		memset (kpage + page_read_bytes, 0, page_zero_bytes);

		/* Add the page to the process's address space. */
		if (!install_page (upage, kpage, writable)) {
			printf("fail\n");
			palloc_free_page (kpage);
			return false;
		}

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* Create a minimal stack by mapping a zeroed page at the USER_STACK */
static bool
setup_stack (struct intr_frame *if_) {
	uint8_t *kpage;
	bool success = false;

	kpage = palloc_get_page (PAL_USER | PAL_ZERO);
	if (kpage != NULL) {
		success = install_page (((uint8_t *) USER_STACK) - PGSIZE, kpage, true);
		if (success)
			if_->rsp = USER_STACK;
		else
			palloc_free_page (kpage);
	}
	return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
 * virtual address KPAGE to the page table.
 * If WRITABLE is true, the user process may modify the page;
 * otherwise, it is read-only.
 * UPAGE must not already be mapped.
 * KPAGE should probably be a page obtained from the user pool
 * with palloc_get_page().
 * Returns true on success, false if UPAGE is already mapped or
 * if memory allocation fails. */
static bool
install_page (void *upage, void *kpage, bool writable) {
	struct thread *t = thread_current ();

	/* Verify that there's not already a page at that virtual
	 * address, then map our page there. */
	return (pml4_get_page (t->pml4, upage) == NULL
			&& pml4_set_page (t->pml4, upage, kpage, writable));
}
#else
/* From here, codes will be used after project 3.
 * If you want to implement the function for only project 2, implement it on the
 * upper block. */

static bool
lazy_load_segment (struct page *page, void *aux) {
	/* TODO: Load the segment from the file */
	/* TODO: This called when the first page fault occurs on address VA. */
	/* TODO: VA is available when calling this function. */
}

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	while (read_bytes > 0 || zero_bytes > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* TODO: Set up aux to pass information to the lazy_load_segment. */
		void *aux = NULL;
		if (!vm_alloc_page_with_initializer (VM_ANON, upage,
					writable, lazy_load_segment, aux))
			return false;

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* Create a PAGE of stack at the USER_STACK. Return true on success. */
static bool
setup_stack (struct intr_frame *if_) {
	bool success = false;
	void *stack_bottom = (void *) (((uint8_t *) USER_STACK) - PGSIZE);

	/* TODO: Map the stack on stack_bottom and claim the page immediately.
	 * TODO: If success, set the rsp accordingly.
	 * TODO: You should mark the page is stack. */
	/* TODO: Your code goes here */

	return success;
}
#endif /* VM */
