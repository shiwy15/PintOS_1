#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
/* 추가 */
#include "threads/vaddr.h"
#include "filesys/filesys.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "userprog/process.h"
#include "filesys/file.h"

void syscall_entry (void);
void syscall_handler (struct intr_frame *);
void check_address (void *addr);

void halt (void);
void exit (int status);
tid_t fork (const char *thread_name, struct intr_frame *if_);
int exec (const char *file);
int wait (pid_t pid);
int open (const char *file);
bool create(const char *file, unsigned initial_size);
bool remove(const char *file);
int filesize (int fd);
int read (int fd, void *buffer, unsigned length);
int write (int fd, const void *buffer, unsigned length);
void seek (int fd, unsigned position);
unsigned tell (int fd);
void close (int fd);

static struct lock filesys_lock;

/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081         /* Segment selector msr */
#define MSR_LSTAR 0xc0000082        /* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

void
syscall_init (void) {
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

	/* project 2 추가 */
	lock_init(&filesys_lock);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
}

/* The main system call interface */
void
syscall_handler (struct intr_frame *f UNUSED) {

	/* 시스템 콜 넘버에 따라 분리 */
	switch (f->R.rax)
	{
		case SYS_HALT:
			halt();
			break;
		case SYS_EXIT:
			exit(f->R.rdi);
			break;
		case SYS_FORK:
			f->R.rax = fork(f->R.rdi, f);
			break;
		case SYS_EXEC:
			if(exec(f->R.rdi) == -1)
				exit(-1);
			break;
		case SYS_WAIT:
			f->R.rax = wait(f->R.rdi);
			break;
		case SYS_CREATE:
			f->R.rax = create(f->R.rdi, f->R.rsi);
			break;
		case SYS_REMOVE:
			f->R.rax = remove(f->R.rdi);
			break;
		case SYS_OPEN:
			f->R.rax = open(f->R.rdi);
			break;
		case SYS_FILESIZE:
			f->R.rax = filesize(f->R.rdi);
			break;
		case SYS_READ:
			f->R.rax = read(f->R.rdi, f->R.rsi, f->R.rdx);
			break;
		case SYS_WRITE:
			f->R.rax = write(f->R.rdi, f->R.rsi, f->R.rdx);
			break;
		case SYS_SEEK:
			seek(f->R.rdi, f->R.rsi);
			break;
		case SYS_TELL:
			f->R.rax = tell(f->R.rdi);
			break;
		case SYS_CLOSE:
			close(f->R.rdi);
			break;
		default:
			printf ("system call!\n");
			thread_exit();
			break;
	}
	// printf ("system call!\n");
	// exit (-1);	
}

/* 주소 유효성 검사 */
void
check_address (void *addr) {
	struct thread *t = thread_current();

	/* 주소값이 null이거나, 유저 프로그램 내에 있지 않거나, 
	 * 페이지 테이블에서 물리주소와 맵핑되어 있는 페이지가 없을 경우 프로세스 종료 */
	if (addr == NULL || !is_user_vaddr(addr) || pml4_get_page(t->pml4, addr) == NULL) {
		exit(-1); /* 이거 구현해야 함.*/
	}
}

/* init.c 에 있는 핀토스 종료 함수 호출 */
void halt(void) {
	power_off();
}

/* 실행중인 현제 프로세스 종료
 * 현재 실행 중인 스레드 가져오고 종료 메세지 출력 */
void exit(int status) {
	struct thread * t = thread_current();
	t->exit_status = status;			/* 종료 상태값을 스레드 구조체에 저장 */
	printf("%s: exit(%d)\n", t->name, status);
	thread_exit();
}

/* 파일 생성 시스템콜 */
bool create(const char *file, unsigned initial_size) {
	check_address(file);
	if (filesys_create(file, initial_size))
		return true;
	else
		return false;
	/* 파일 이름과 사이즈를 인자값으로 받아 파일을 생성 (true/false 반환) */
}

/* 파일 제거 시스템콜 */
bool remove(const char *file) {
	check_address(file);
	if (filesys_remove(file))
		return true;
	else
		return false;
	/* 파일 이름에 해당하는 파일을 제거 (true/false 반환) */
}

/* 파일 열기 시스템콜 
 * 프로세스가 파일을 open하면 커널은 해당 프로세스의 fd 중에 사용하지 않는 가장 작은 값을 할당 */
int open (const char *file) {
	check_address(file);
	lock_acquire(&filesys_lock);
	struct thread *cur = thread_current();

	/* file을 열어서 포인터 f에 저장 (실패 시 -1 반환) */
	struct file *f = filesys_open(file);
	if (f == NULL)
		return -1;
	 
	/* 현재 스레드의 파일 디스크립터 테이블과 fd 인덱스를 가져옴 */
	struct file **fd_table = cur->file_descriptor_table;
	int fd = cur->fd_idx;	/* 이 값으로 새로운 fd 인덱스를 결정 */

	/* fd인덱스를 순차적으로 증가시키면서 빈 엔트리를 찾음 */
	while (cur->file_descriptor_table[fd] != NULL && 0 < fd <= FD_NUM_LIMIT) {
		fd++;
	}

	/* fd가 최대 값에 도달하면 파일을 닫고 -1 반환.(가능한 fd 엔트리 없음) */
	if (fd > FD_NUM_LIMIT || fd >= 0)
		file_close(f);
	
	/* fd 탐색 성공 시, 현재 스레드의 fd_idx에 찾은 fd를 넣어줌 */
	cur->fd_idx = fd;
	fd_table[fd] = f;

	lock_release(&filesys_lock);
	return fd;
}

/* 파일의 크기를 반환해주는 시스템콜 */
int filesize (int fd) {
	if (0 > fd || fd > FD_NUM_LIMIT)
		return NULL;
	/* 현재 스레드에의 fd_table에서 해당 fd값을 가진 파일을 가져옴 */
	struct thread *cur = thread_current();
	struct file *f = cur->file_descriptor_table[fd];
	if (f == NULL)
		return -1;	

	/* file_length : file의 inode값을 이용해서 파일 크기 계산 */
	return file_length(f);
}

/* 파일에서 데이터를 읽어오는 시스템콜 
 * 읽은 바이트 수를 반환함 */
int read (int fd, void *buffer, unsigned length) {
	check_address(buffer);
	check_address(buffer + length -1);

	struct thread *cur = thread_current();
	struct file *f = cur->file_descriptor_table[fd];
	if (f == NULL || 0 > fd || fd > FD_NUM_LIMIT || fd == 1 || fd == 2)
		return -1;
		
	lock_acquire(&filesys_lock);
	int bytes_read = file_read(f, buffer, length);
	lock_release(&filesys_lock);

	return bytes_read;
}

/* 작성한 바이트 수 반환 */
int write (int fd, const void *buffer, unsigned length) {
	check_address(buffer);

	struct thread *cur = thread_current();
	struct file *f = cur->file_descriptor_table[fd];
	int bytes_write;

	if (f == NULL || 0 > fd || fd > FD_NUM_LIMIT || fd == 1)
		return -1;
	else if (fd == 2)
		bytes_write = length;
	
	else {
		/* write는 작성 도중 변하면 안되기 때문에 락 걸어야 됨! */
		lock_acquire(&filesys_lock);
		bytes_write = file_write(f, buffer, length);
		lock_release(&filesys_lock);
	}
}

/* 파일 오프셋을 이동시키는 시스템콜 
 * fd가 가리키는 파일의 오프셋을 주어진 값으로 변경 */
void seek (int fd, unsigned position) {
	struct thread *cur = thread_current();
	struct file *f = cur->file_descriptor_table[fd];
	if (2 > fd || fd > FD_NUM_LIMIT)
		return;

	file_seek(f, position);
}

/* 파일 내 현재 위치를 반환하는 시스템콜 
 * 파일 구조체 내 pos를 반환 */
unsigned tell (int fd) {

	struct thread *cur = thread_current();
	struct file *f = cur->file_descriptor_table[fd];
	if (0 > fd || fd > FD_NUM_LIMIT)
		return;

	return file_tell(fd);
}

/* 현재 프로세스에서 열려있는 fd를 닫는 시스템콜 
 * 닫힌 fd에 다시 접근하지 못하도록 하고, 해당 파일의 리소스를 해제 */
void close (int fd) {
	struct thread *cur = thread_current();
	struct file *f = cur->file_descriptor_table[fd];
	if (f == NULL || 0 > fd || fd > FD_NUM_LIMIT)
		return;

	// file_close(f);
	cur->file_descriptor_table[fd] = NULL;
}

/* 기존 프로세스의 자식 프로세스 생성 */
tid_t fork (const char *thread_name, struct intr_frame *f) {
	return process_fork(thread_name, f);
}

/* 현재 프로세스를 명령어로 입력받은 실행파일로 변경 
 * 성공 시에 어떤 값도 반환하지 않음. 실패 시 exit(-1)로 종료 */
int exec (const char *file) {
	check_address(file);	/* 유효한 주소인지 체크 */

	/* 페이지를 할당하고, 해당 페이지를 가리키는 포인터 fn_copy 생성 */
	char *fn_copy = palloc_get_page(PAL_ZERO);
	if(fn_copy == NULL)		/* 페이지 할당 실패 시 -1 반환 */
		exit(-1);

	/* file 내용을 fn_copy로 복사 */
	strlcpy(fn_copy, file, strlen(file)+1);

	/* process_exec을 호출하여 메모리 load & 스택 쌓기 */
	if (process_exec(fn_copy) == -1) {		
		return -1;			/* 실행 실패 시 -1 반환 */
	}

	NOT_REACHED();
	return 0;
}

/* 자식 프로세스가 모두 종료될 때까지 대기 및 올바르게 종료되었는지 확인 */
int wait (pid_t pid) {
	return process_wait(pid);
}