#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
#include "threads/vaddr.h"
#include "filesys/filesys.h"
#include "userprog/process.h"

void syscall_entry(void);
void syscall_handler(struct intr_frame *);

void check_address(uaddr);
static int64_t get_user(const uint8_t *uaddr);
static bool put_user(uint8_t *udst, uint8_t byte);
void halt(void);
void exit(int status);
bool create(const char *file, unsigned initial_size);
bool remove(const char *file);
int read(int fd, void *buffer, unsigned size);
int write(int fd, const void *buffer, unsigned size);

/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081			/* Segment selector msr */
#define MSR_LSTAR 0xc0000082		/* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

void syscall_init(void)
{
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48 |
							((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t)syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
			  FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
}

/* The main system call interface */
// #define DEBUG
void syscall_handler(struct intr_frame *f UNUSED)
{
	// TODO: Your implementation goes here.

	//printf("system call!\n");
	//printf("syscall : %d\n", f->R.rax);

#ifdef DEBUG
	// syscall stack frame? (stack base - stack ptr)
	//hex_dump(f->rsp, f->rsp, f->R.rbp - f->rsp, true); // #ifdef DEBUG

	// write sys-call
	hex_dump(f->R.rsi, f->R.rsi, f->R.rdx, true); // #ifdef DEBUG

	// print whole user stack (rsp ~)
	// hex_dump(f->rsp, f->rsp, USER_STACK - f->rsp, true); // #ifdef DEBUG
	printf("==================\n");

	// print rbp ~
	//hex_dump(f->R.rbp, f->R.rbp, USER_STACK - f->R.rbp, true); // #ifdef DEBUG
#endif

	// #ifdef DEBUG
	char *fn_copy;
	int siz;
	bool writable;
	// SYS_EXEC - process_exec의 process_cleanup 때문에 f->R.rdi 날아감.
	// 여기서 동적할당해서 복사한 뒤, 그걸 넘겨주기?

	switch (f->R.rax)
	{
	case SYS_HALT:
		halt();
		break;
	case SYS_EXIT:
		exit(f->R.rdi);
		break;
	case SYS_FORK:
		f->R.rax = process_fork(f->R.rdi, NULL);
		break;
	case SYS_EXEC:
		// writable = is_kernel_vaddr(f->R.rdi); //is_writable((uint64_t *)f->R.rdi);
		// fn_copy = palloc_get_page(0);
		// if (fn_copy == NULL)
		// 	exit(-1);
		// siz = strlen(f->R.rdi);
		// strlcpy(fn_copy, f->R.rdi, siz); // Kernel panic; fn_copy는 kernel virtual addr라 write가 안되는건가?

		if (process_exec(f->R.rdi) == -1)
			exit(-1);
		break;
	case SYS_WAIT:
		f->R.rax = process_wait(f->R.rdi);
		break;
	case SYS_CREATE:
		f->R.rax = create(f->R.rdi, f->R.rsi);
		break;
	case SYS_REMOVE:
		f->R.rax = remove(f->R.rdi);
		break;
	case SYS_OPEN:
		break;
	case SYS_FILESIZE:
		break;
	case SYS_READ:
		f->R.rax = read(f->R.rdi, f->R.rsi, f->R.rdx);
		break;
	case SYS_WRITE:
		f->R.rax = write(f->R.rdi, f->R.rsi, f->R.rdx);
		break;
	case SYS_SEEK:
		break;
	case SYS_TELL:
		break;
	case SYS_CLOSE:
		break;
	default:
		//thread_exit();
		exit(-1);
		break;
	}

	//thread_exit();
}

/* Just check whether the address is under KERN_BASE */
void check_address(const uint64_t *uaddr)
{
	if (!(is_user_vaddr(uaddr)))
	{
		exit(-1);
	}
	// Q. page_fault 함수 호출?
}

/* Reads a byte at user virtual address UADDR.
 * UADDR must be below KERN_BASE.
 * Returns the byte value if successful, -1 if a segfault
 * occurred. */
static int64_t
get_user(const uint8_t *uaddr)
{
	int64_t result;
	__asm __volatile(
		"movabsq $done_get, %0\n"
		"movzbq %1, %0\n"
		"done_get:\n"
		: "=&a"(result)
		: "m"(*uaddr));
	return result;
}

/* Writes BYTE to user address UDST.
 * UDST must be below KERN_BASE.
 * Returns true if successful, false if a segfault occurred. */
static bool
put_user(uint8_t *udst, uint8_t byte)
{
	int64_t error_code;
	__asm __volatile(
		"movabsq $done_put, %0\n"
		"movb %b2, %1\n"
		"done_put:\n"
		: "=&a"(error_code), "=m"(*udst)
		: "q"(byte));
	return error_code != -1;
}

void halt(void)
{
	power_off();
}

void exit(int status)
{
	struct thread *cur = thread_current();
	cur->exit_status = status;

	printf("%s: exit(%d)\n", thread_name(), status);
	thread_exit();
}

bool create(const char *file, unsigned initial_size)
{
	if (file == NULL) exit(-1);
	check_address(file);
	return filesys_create(file, initial_size);
}

bool remove(const char *file)
{
	return filesys_remove(file);
}

int read(int fd, void *buffer, unsigned size)
{
	check_address(buffer);
	if (fd == 0)
	{
		int i;
		unsigned char *buf = buffer;
		for (i = 0; i < size; i++)
		{
			char c = input_getc();
			*buf++ = c;
			if (c == '\0')
				break;
		}
		return i;
	}
}

int write(int fd, const void *buffer, unsigned size)
{
	check_address(buffer);
	if (fd == 1)
	{
		putbuf(buffer, size);
		return size;
	}
	return -1;
}

tid_t fork(const char *thread_name)
{
	// create new process (thread?) with the given name -> thread_create? process_create_initd?
	tid_t tid = process_create_initd(thread_name);
	if (tid == TID_ERROR)
		return TID_ERROR;

	// clone callee-saved registers
	struct thread *parent = thread_current();
	struct thread *child = get_child_with_pid(tid);
	child->tf.rsp = parent->tf.rsp;
	child->tf.R.rbx = parent->tf.R.rbx;
	child->tf.R.rbp = parent->tf.R.rbp;
	child->tf.R.r12 = parent->tf.R.r12;
	child->tf.R.r13 = parent->tf.R.r13;
	child->tf.R.r14 = parent->tf.R.r14;
	child->tf.R.r15 = parent->tf.R.r15;

	// duplicate resources - files descriptors (fd?) and VM space (pml4_for_each)
	child->pml4 = parent->pml4;
	//pml4_for_each(parent->pml4, copy_page, child);

	// child return val
	child->tf.R.rax = 0;
	return tid;
}

// static bool
// copy_page (uint64_t *pte, void *va,  void *aux) {
//         if (is_user_vaddr (va)){
// 			child->pml4
// 		}
// 			printf ("user page: %llx\n", va);
//         return true;
// }