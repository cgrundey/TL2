#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/sem.h>

#include <errno.h>

#define CFENCE  __asm__ volatile ("":::"memory")
#define MFENCE  __asm__ volatile ("mfence":::"memory")

volatile unsigned int lock;

#define IS_LOCKED(lock) lock & 1 == 1

#define UNLOCK(lock, new_ver) lock = new_ver << 1

#define GET_VERSION(lock) lock >> 1

#define IS_UNCHANGED(old_lock_val, lock)  (old_lock_val & ~1) == lock

#define TRY_LOCK(lock) __sync_bool_compare_and_swap(&(lock), (lock) & ~1, lock | 1)

int main(int argc, char* argv[])
{
	lock = 0;
	
	printf("Is locked? %d\n", IS_LOCKED(lock));
	
	printf("Try lock? %d\n", TRY_LOCK(lock));

	printf("Is locked? %d\n", IS_LOCKED(lock));

	printf("Try lock again? %d\n", TRY_LOCK(lock));

	UNLOCK(lock, 10);

	printf("Unlocked with version 10. So, version = %d\n", GET_VERSION(lock));

	unsigned int v1 = lock;
	CFENCE;
	//Read val in tx_read
	CFENCE;
	printf("Is unchanged (should be true)? %d\n", IS_UNCHANGED(v1, lock));

	printf("Try lock? %d\n", TRY_LOCK(lock));

	printf("Is unchanged now? %d\n", IS_UNCHANGED(v1, lock));

	UNLOCK(lock, 15);

	printf("Unlocked with version %d, is unchaned? %d\n", GET_VERSION(lock), IS_UNCHANGED(v1, lock));

	return 0;
}
