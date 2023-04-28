#include "devices/timer.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include "threads/interrupt.h"
#include "threads/io.h"
#include "threads/synch.h"
#include "threads/thread.h"

/* See [8254] for hardware details of the 8254 timer chip. 
	- 8254 타이머 칩은 컴퓨터 시스템에서 타이밍과 관련된 기능을 수행하는 칩
	- Pintos에서 하드웨어 타이머를 사용하여 시스템의 시간을 계산하는데, 이때 8254 칩 사용*/
/* ❗️OS에서 사용하는 8254 타이머 칩을 초기화 
	- Pintos 운영체제가 올바르게 작동하기 위해 타이머의 빈도를 정해 놓음
	- Timer_Freq(빈도 수)가 19 이상, 1000 이하의 값이 권장됨을 의미 */
#if TIMER_FREQ < 19
#error 8254 timer requires TIMER_FREQ >= 19
#endif
#if TIMER_FREQ > 1000
#error TIMER_FREQ <= 1000 recommended
#endif

/* Number of timer ticks since OS booted. 
	- ❗️전역변수 ticks를 정의 : 운영제제가 부팅 된 후 경과한 timer ticks의 수
	- 하드웨어 타이머에서 발생하는 일종의 시계 신호. Pintos의 시스템 시간을 나타냄.*/
static int64_t ticks;

/* Number of loops per timer tick. Initialized by timer_calibrate(). 
   - ❗️전역변수 loops_per_tick 정의 : 타이머 인터럽트를 처리하는데 사용되는 루프 수
   - 타이머 인터럽트는 정해진 시간마다 발생하는 신호. 운영체제가 다음 작업을 수행하기 전에 일정한 시간이 지났는지 확인하고,
	필요한 경우 해당 작업을 중지하고 다른 작업으로 전환할 수 있도록 함
   - Pintos에서는 8254 타이머 칩을 이용해 타이머 인터럽트를 구현하고 있음. 
   	이 칩은 운영체제가 정확한 시간을 계산할 수 있도록 일정한 주기로 신호를 발생시키며, 실제 시간과 인터럽트의 발생 주기가 일치하도록 조정하기 위해
	timer_calibrate() 함수를 이용해 loop_per_tick 값이 계산됨
   - loop_per_tick 값은 타이머 인터럽트가 발생할 때마다 카운터를 감소시키고, 카운터가 0이 되면 타이머 인터럽트를 발생시킴
   	이를 통해 운영체제가 일정한 주기로 타이머 인터럽트를 받아들이고, 이를 기반으로 시간을 계산하게 됨.(시스템 콜이나 스케줄링 등에 사용됨)*/
static unsigned loops_per_tick;

/* ❗️타이머 관련 핸들러와 함수들 */
static intr_handler_func timer_interrupt;  /* 타이머 인터럽트 핸들러 함수. 인터럽트 발생 시 핸들러가 실행됨 */
static bool too_many_loops (unsigned loops);  /* busy-waiting에서 사용 */
static void busy_wait (int64_t loops);  
static void real_time_sleep (int64_t num, int32_t denom);  /* 대기시간을 계산하고 해당 대기시간 동안 sleep 상태로 전환 */

/* Sets up the 8254 Programmable Interval Timer (PIT) to
   interrupt PIT_FREQ times per second, and registers the
   corresponding interrupt. 
   ❗️타이머를 초기화하고 인터럽트를 등록하는 함수
   	- 8254 Programmable Interval Timer(PIT) : 시스템에서 일정 시간 간격으로 인터럽트를 발생시키는 하드웨어 장치
	- timer_int 함수는 PIT을 초기화하고, Timer_Freq 횟수만큼 인터럽트하도록 설정*/
void
timer_init (void) {
	/* 1. PIT을 timer_freq에 맞게 설정
		- PIT의 입력 주파수(1193180)를 Timer_Freq으로 나눔. '(1193180 + TIMER_FREQ / 2)'은 반올림을 위해 더해지는 부분 */
	uint16_t count = (1193180 + TIMER_FREQ / 2) / TIMER_FREQ; 

	/* 2. PIT을 초기화
		- Out()은 x86 아키텍처의 I/O 포트에 바이트를 출력하는 함수. (하드웨어에 바이트 전송 가능)
		- void outb (uint16_t port, uint8_t value); (port는 출력할 I/O 포트번호, value는 출력할 바이트 값)
		- 아래에서는 out 함수로 8254 타이머 칩에 명령어를 전송 */
	outb (0x43, 0x34);    /* CW: counter 0, LSB then MSB, mode 2, binary. */
	outb (0x40, count & 0xff);
	outb (0x40, count >> 8);

	/* 3. 커널이 인터럽트를 처리할 수 있도록 핸들러 함수 등록 
		- intr_register_ext()는 인터럽트를 처리하는 핸들러 함수를 등록하는 함수
		- 아래 코드는 0x20번 인터럽트에 대한 핸들러 함수로 'timer_interrupt'함수를 등록하고 "8254 Timer"이라는 이름을 붙인다는 뜻 */
	intr_register_ext (0x20, timer_interrupt, "8254 Timer");
}

/* Calibrates loops_per_tick, used to implement brief delays. 
	❗️ loops_per_tick 값을 계산하여, 짧은 딜레이를 구현하는데 사용!
	- 먼저 1틱당 반복되는 루프 수를 계산하고, too_many_loops 함수로 루프 수가 너무 많은지 확인
	- 10번째 비트부터 최대 18번째 비트까지 차례로 검사하며, 이 과정을 마치면 loops_per_tick 변수가 정확한 값을 가지게 됨
	- 최종적으로는 loops_per_tick 값을 기반으로 초당 루프 수를 계산 한 뒤 출력함 */
void
timer_calibrate (void) {
	unsigned high_bit, test_bit;

	/* 1. 디버깅 코드
	- intr_get_level은 현재 인터럽트의 상태(활성/비활성) 반환
	- intr_on 은 인터럽트가 활성화된 상태. 즉, 아래 코드는 인터럽트가 활성화된 상태에서만 timer_calibrate 함수를 호출하도록 보장 */
	ASSERT (intr_get_level () == INTR_ON);
	printf ("Calibrating timer...  ");

	/* Approximate loops_per_tick as the largest power-of-two
	   still less than one timer tick. */
	/* 2. loops_per_tick 값을 대략적으로 설정 : 2의 거듭제곱으로 늘려가면서 설정 */
	loops_per_tick = 1u << 10;  /* 해당 변수를 2의 10승 == 1024로 초기화 (u = unsigned)*/
	/* too_many_loops가 false를 반환하는 동안, loops_per_tick 변수를 2배씩 늘려감 */
	while (!too_many_loops (loops_per_tick << 1)) {  
		loops_per_tick <<= 1; 
		ASSERT (loops_per_tick != 0);  /* 루프 변수가 0이 되는 것을 방지 */
	}

	/* Refine the next 8 bits of loops_per_tick. */
	/* 3. loops_per_tick 값을 보다 정확하게 재설정 
		- high_bit은 현재 loops_per_tick의 값을 복사하고, test_bit은 high_bit의 다음 8비트를 가리키는 포인터 역할
		- test_bit을 한 비트씩 이동하면서 loops_per_tick을 보완*/
	high_bit = loops_per_tick;
	for (test_bit = high_bit >> 1; test_bit != high_bit >> 10; test_bit >>= 1)
		if (!too_many_loops (high_bit | test_bit))
			loops_per_tick |= test_bit;

	/* 4. loops_per_tick 값에 대한 정보 출력 */
	printf ("%'"PRIu64" loops/s.\n", (uint64_t) loops_per_tick * TIMER_FREQ);
}

/* Returns the number of timer ticks since the OS booted.
	❗️OS가 부팅한 이후로 지나간 타이머 tick의 수를 반환하는 함수 */
int64_t
timer_ticks (void) {
	enum intr_level old_level = intr_disable ();  /* 현재 인터럽트 레벨을 저장하고, 인터럽트를 비활성화 */
	// 미리 인터럽트 레벨을 비활성화해야 ticks값이 중간에 업데이트되어 잘못된 값을 반환하는 일이 없음
	int64_t t = ticks;  /* 전역변수 ticks의 값을 t에 저장 */
	intr_set_level (old_level);  /* 인터럽트 레벨을 이전 값으로 복원 */
	barrier ();  /* 코드 실행의 순서가 변경되는 것을 방지 */
	return t;  /* 지나간 타이머 tick의 수 t를 반환 */
}

/* Returns the number of timer ticks elapsed since THEN, which
   should be a value once returned by timer_ticks(). 
   ❗️이전에 반환된 timer_ticks() 값과 현재 ticks 값을 비교하여, then 값 이후에 경과한 타이머 틱의 수를 계산해서 반환하는 함수 */
int64_t
timer_elapsed (int64_t then) {
	return timer_ticks () - then;
}

/* Suspends execution for approximately TICKS timer ticks. 
	❗️현재 실행중인 스레드를 인자로 받은 ticks만큼 잠들게 하는 함수
	- 이 코드의 문제점 : 잠들게 한 이후 얼마나 시간이 흘렀는지를 계속 ticks와 비교하면서 일어날 시간을 확인함.*/
void
timer_sleep (int64_t ticks) {
	int64_t start = timer_ticks ();  /* 슬립 시작 시점의 tick을 저장 */

	ASSERT (intr_get_level () == INTR_ON);  /* 디버깅 코드 : 인터럽트가 활성화된 상태에서만 함수 실행 */
	thread_sleep (start + ticks);
}

/* Suspends execution for approximately MS milliseconds. 
	❗️주어진 시간(ms)만큼 실행을 멈추는 함수 */
void
timer_msleep (int64_t ms) {
	real_time_sleep (ms, 1000);
}

/* Suspends execution for approximately US microseconds. 
	❗️입력받은 마이크로초(us)만큼 실행을 멈추는 함수 */
void
timer_usleep (int64_t us) {
	real_time_sleep (us, 1000 * 1000);
}

/* Suspends execution for approximately NS nanoseconds. 
	❗️나노초(ns)만큼 실행을 멈추는 함수 */
void
timer_nsleep (int64_t ns) {
	real_time_sleep (ns, 1000 * 1000 * 1000);
}

/* Prints timer statistics.
	❗️현재까지 발생한 타이머 틱의 수를 출력하는 함수 : 지금까지 얼마나 많은 틱이 발생했는지 알 수 있음 */
void
timer_print_stats (void) {
	printf ("Timer: %"PRId64" ticks\n", timer_ticks ());
}

/* Timer interrupt handler. 
	❗️타이머 인터럽트가 발생할 때마다 호출되는 핸들러 함수 
	- 타이머 인터럽트는 운영체제에서 일정 주기마다 발생
	- 현재 시간을 나타내는 변수인 ticks를 증가시키고, thread_tick()함수를 호출 
	- 스케줄링 시 중요한 역할 */
static void
timer_interrupt (struct intr_frame *args UNUSED) {
	ticks++;
	thread_tick (); 
	/* 🚨 alarm clock 함수 추가 */
	thread_awake (ticks); 	/* ticks가 증가할 때마다 awake로 깨울 스레드가 있는지 체크 */
}

/* Returns true if LOOPS iterations waits for more than one timer
   tick, otherwise false. 
   ❗️주어진 루프 만큼 busy-wait을 수행하고, 타이머 틱 하나가 발생하는데 걸리는 시간이 초과되었는지 확인하는 함수 */
static bool
too_many_loops (unsigned loops) {  
	/* Wait for a timer tick. */
	int64_t start = ticks;  // start 변수 초기화
	/* 현재 타이머 틱의 수가 start와 같은 동안 계속 반복문 수행 
		--> 하나의 타이머 틱이 지날 때까지 대기하는 역할 */
	while (ticks == start)
		barrier ();  /* 메모리 배리어 : 코드 재배치나 최적화로 발생할 수 있는 문제를 방지 */

	/* Run LOOPS loops. */
	start = ticks;  /* busy-wait 수행 전 타이머 틱의 수를 저장 */
	busy_wait (loops);  /* 주어진 loop만큼 busy-wait 수행 */

	/* If the tick count changed, we iterated too long.*/
	/* start 값과 현재 틱이 다르면 반목문 수행시간이 타이머 틱을 초과했다는 의미이므로 ture를 반환 */
	barrier ();
	return start != ticks;  
}

/* Iterates through a simple loop LOOPS times, for implementing
   brief delays.

   Marked NO_INLINE because code alignment can significantly
   affect timings, so that if this function was inlined
   differently in different places the results would be difficult
   to predict. */
/* ❗️busy waiting을 구현하는 함수 : 루프의 반복횟수를 기반으로 짧은 딜레이를 만들기 위해 사용 
	- NO_INLINE으로 마크되어 있음. 코드 정렬이 실행시간에 큰 영향을 미치기 때문에 인라인될 경우 예측하기 어려운 결과가 나타날 수 있음
	- 따라서 컴파일러에 의해 인라인 되지 않도록 미리 방지
	- 인라인 : 함수를 호출하는 대신, 함수의 내용을 호출하는 부분에 직접 삽입하는 것 */
static void NO_INLINE
busy_wait (int64_t loops) {
	while (loops-- > 0)
		barrier ();
}

/* Sleep for approximately NUM/DENOM seconds. 
	❗️num/denom 초 동안 슬립하는 기능을 제공하는 함수 */
static void
real_time_sleep (int64_t num, int32_t denom) {
	/* Convert NUM/DENOM seconds into timer ticks, rounding down.

	   (NUM / DENOM) s
	   ---------------------- = NUM * TIMER_FREQ / DENOM ticks.
	   1 s / TIMER_FREQ ticks
	   */
	/* num/denom 초를 타이머 틱으로 변환하여 몇 개의 틱을 기다려야 하는지 계산 */
	int64_t ticks = num * TIMER_FREQ / denom;

	ASSERT (intr_get_level () == INTR_ON);
	/* 만약 기다려야할 틱이 한 개 이상이면, timer_sleep() 함수를 사용하여 cpu를 다른 프로세스에게 양도하고 기다림 */
	if (ticks > 0) {
		/* We're waiting for at least one full timer tick.  Use
		   timer_sleep() because it will yield the CPU to other
		   processes. */
		timer_sleep (ticks);
	} else {
		/* Otherwise, use a busy-wait loop for more accurate
		   sub-tick timing.  We scale the numerator and denominator
		   down by 1000 to avoid the possibility of overflow. */
		/* 기다려야할 틱이 없으면, busy wait루프를 사용하는데, denom을 1000으로 나누면 오버플로우 가능성을 피할 수 있음 */
		ASSERT (denom % 1000 == 0);  /* 디버깅 코드 : denom이 1000의 배수인지 검사 */
		busy_wait (loops_per_tick * num / 1000 * TIMER_FREQ / (denom / 1000));
		/* num/denom초를 마이크로초 단위로 변환하여 계산 */
	}
}
