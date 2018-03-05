/**
* Software Transactional Memory 2
* Disjoint threads
*
* Author: Colin Grundey
* Date: February 26, 2018
*
* Compile:
*   g++ grundeyTL2disjoint.cpp -o TL2disjoint -lpthread
*   add -g option for gdb
*/

#include <pthread.h>
#include <list>
#include <cstdlib>
#include <vector>
#include <signal.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <iostream>
#include <time.h>
#include <unordered_map>
#include <list> // Linked list for read/write sets

#include <errno.h>

#include "rand_r_32.h"

#define CFENCE  __asm__ volatile ("":::"memory")
#define MFENCE  __asm__ volatile ("mfence":::"memory")

#define IS_LOCKED(lock) lock & 1 == 1

#define UNLOCK(lock, new_ver) lock = new_ver << 1

#define GET_VERSION(lock) lock >> 1

#define IS_UNCHANGED(old_lock_val, lock)  (old_lock_val & ~1) == lock

#define TRY_LOCK(lock) __sync_bool_compare_and_swap(&(lock), (lock) & ~1, lock | 1)

#define NUM_ACCTS    1000000
#define NUM_TXN      100000
#define TRFR_AMT     50
#define INIT_BALANCE 1000

using namespace std;

typedef struct {
  int addr;
  int value;
} Acct;

int accts[NUM_ACCTS];
volatile unsigned int myLocks[NUM_ACCTS];
int numThreads;
thread_local list<Acct> read_set;
thread_local list<Acct> write_set;
thread_local long rv = 0;
thread_local long wv = 0;
long global_clock = 0;

static volatile long abort_count = 0;
static volatile long commit_count = 0;

/* Where a transaction begins. Read and write sets are intialized and cleared. */
void tx_begin() {
  read_set.clear();
  write_set.clear();
  rv = global_clock;
}
/* Aborts from transaction by throwing ann exception of type STM_exception */
void tx_abort() {
  list<Acct>::iterator iterator;
  for (iterator = read_set.begin(); iterator != read_set.end(); ++iterator)
    UNLOCK(myLocks[iterator->addr], GET_VERSION(myLocks[iterator->addr]));
  for (iterator = write_set.begin(); iterator != write_set.end(); ++iterator)
    UNLOCK(myLocks[iterator->addr], GET_VERSION(myLocks[iterator->addr]));
  __sync_fetch_and_add(&abort_count, 1);
  throw "Transaction ABORTED";
}
/* Adds account with version to read_set and returns the value for later use. */
int tx_read(int addr) {
  // Look in write_set first
  list<Acct>::reverse_iterator iterator;
  for (iterator = write_set.rbegin(); iterator != write_set.rend(); ++iterator) {
    if (iterator->addr == addr) {
      read_set.push_back(*iterator);
      return iterator->value;
    }
  } // Must be in memory (i.e. vector of Accts)
  int v1, v2, the_val;
  v1 = GET_VERSION(myLocks[addr]);
  CFENCE;
  the_val = accts[addr];
  CFENCE;
  v2 = GET_VERSION(myLocks[addr]);

  if (v2 <= rv && v1 == v2 && TRY_LOCK(myLocks[addr]) == 0) {
		Acct temp = {addr, accts[addr]};
    read_set.push_back(temp);
    return the_val;
  }
  else {
    tx_abort();
	}
}

void tx_write(int addr, int val) {
  Acct temp = {addr, val};
  write_set.push_back(temp);
}
/* Attempts to commit the transaction */
void tx_commit() {
  list<Acct>::iterator iterator;
  /* Validate write_set */
  for (iterator = write_set.begin(); iterator != write_set.end(); ++iterator) {
    if (TRY_LOCK(myLocks[iterator->addr])) {
			tx_abort();
		}
  }
  wv = __sync_fetch_and_add(&global_clock, 1);

  /* Validate read_set */
  for (iterator = read_set.begin(); iterator != read_set.end(); ++iterator) {
    if (GET_VERSION(myLocks[iterator->addr]) > rv || TRY_LOCK(myLocks[iterator->addr])) {
      tx_abort();
		}
  }
  /* Validation is a success */
  for (iterator = write_set.begin(); iterator != write_set.end(); ++iterator) {
    accts[iterator->addr] = iterator->value;
    UNLOCK(myLocks[iterator->addr], wv);
  }
	for (iterator = read_set.begin(); iterator != read_set.end(); ++iterator) {
		UNLOCK(myLocks[iterator->addr], GET_VERSION(myLocks[iterator->addr]));
	}
}

inline unsigned long long get_real_time() {
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC_RAW, &time);
    return time.tv_sec * 1000000000L + time.tv_nsec;
}

/* Support a few lightweight barriers */
void barrier(int which) {
    static volatile int barriers[16] = {0};
    CFENCE;
    __sync_fetch_and_add(&barriers[which], 1);
    while (barriers[which] < numThreads) { }
    CFENCE;
}

// Thread function
void* th_run(void * args)
{
  long id = (long)args;
  barrier(0);
  srand((unsigned)time(0));
  if (id == 0)
    return 0;

  list<Acct> read_set;
  list<Acct> write_set;
  bool aborted = false;
  int start;
  int thrange = NUM_ACCTS / numThreads;
  start = thrange * (id-1);

  int workload = NUM_TXN / numThreads;
  for (int i = 0; i < workload; i++) {
// ________________BEGIN_________________
    do {
      aborted = false;
      try {
        tx_begin();
        int r1 = 0;
        int r2 = 0;
        for (int j = 0; j < 10; j++) {
          while (r1 == r2) {
            r1 = start + (rand() % (thrange - 1));
            r2 = start + (rand() % (thrange - 1));
          }
          // Perform the transfer
          int a1 = tx_read(r1);
          if (a1 < TRFR_AMT)
            break;
          int a2 = tx_read(r2);
          tx_write(r1, a1 - TRFR_AMT);
          tx_write(r2, a2 + TRFR_AMT);
          tx_commit();
        }
      } catch(const char* msg) {
        aborted = true;
      }
    } while (aborted);
// _________________END__________________
  }
  return 0;
}

int main(int argc, char* argv[]) {
  // Input arguments error checking
  if (argc != 2) {
    printf("Usage: <# of threads -> 1, 2, or 4\n");
    exit(0);
  } else {
    numThreads = atoi(argv[1]);
    if (numThreads != 1 && numThreads != 2 && numThreads != 4) {
      printf("Usage: <# of threads -> 1, 2, or 4\n");
      exit(0);
    }
  }
  printf("Number of threads: %d\n", numThreads);

  // Initializing 1,000,000 accounts with $1000 each
  for (int i = 0; i < NUM_ACCTS; i++)
    accts[i] = INIT_BALANCE;

  long totalMoneyBefore = 0;
  for (int i = 0; i < NUM_ACCTS; i++)
    totalMoneyBefore += accts[i];

  // Initialize locks
  for (int i = 0; i < NUM_ACCTS; i++)
    myLocks[i] = 0;

  // Thread initializations
  pthread_attr_t thread_attr;
  pthread_attr_init(&thread_attr);

  pthread_t client_th[300];
  long ids = 1;
  for (int i = 0; i < numThreads; i++) {
    pthread_create(&client_th[ids-1], &thread_attr, th_run, (void*)ids);
    ids++;
  }

/* EXECUTION BEGIN */
  unsigned long long start = get_real_time();
  th_run(0);
  // Joining Threads
  for (int i=0; i<ids-1; i++) {
    pthread_join(client_th[i], NULL);
  }
/* EXECUTION END */
  long totalMoneyAfter = 0;
  for (int i = 0; i < NUM_ACCTS; i++)
    totalMoneyAfter += accts[i];

  //printf("Total transfers: %d\n", actual_transfers);
  printf("\nTotal time = %lld ns\n", get_real_time() - start);
  printf("Total Money Before: $%ld\n", totalMoneyBefore);
  printf("Total Money After:  $%ld\n", totalMoneyAfter);
  printf("Abort Count: %ld\n\n", abort_count);

  return 0;
}
