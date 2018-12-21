#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <ucontext.h>
#include <setjmp.h>
#include <sys/types.h>
#include <signal.h>
#include <stdbool.h>
#include <string.h>

/**
* A portable user-level multithreading library based off of:
* Portable Multithreading: The Signal Stack Trick For User-Space Thread Creation
* source: https://dl.acm.org/citation.cfm?id=1267744
*/

#define STACK_SIZE 8192
#define DEBUG 1 // for debugging with print statements
typedef struct mctx_t {
    jmp_buf jb;
} mctx_t;

static mctx_t mctx_caller;
static sig_atomic_t mctx_called;

static mctx_t *mctx_creat;
static void (*mctx_creat_func)(void *);
static void *mctx_creat_arg;
static sigset_t mctx_creat_sigs;

/** 
* macros for manipualting thread contexts 
* source: https://dl.acm.org/citation.cfm?id=1267744
*/
/* Save machine context. ie. getcontext */
#define mctx_save(mctx) setjmp((mctx)->jb)

/* Restore machine context. ie. setcontext */
#define mctx_restore(mctx) longjmp((mctx)->jb, 1)

/* Switch machine context. ie. swapcontext */
#define mctx_switch(mctx_old, mctx_new) if (setjmp((mctx_old)->jb) == 0) longjmp((mctx_new)->jb,1) 


/* global variables to be accessed by each thread */
static jmp_buf end;
mctx_t main_thread; // main thread
mctx_t* uc; // uc is essentially a queue. uc[0] is always the active thread!!
static char** stacks; // define stack space for each uc context
int max_threads = 0; // number of threads we can currently work with (dynamically increased)
int num_threads = 0; // number of contexts to init
int active_threads = 0;

void my_thr_create(int thr_id, void (*func) (void*), void* arg);
void my_thr_start();
void my_thr_yield();
void my_thr_exit();
void _exit_thread();

/* Creating machine context: */
static mctx_t       mctx_caller;
static sig_atomic_t mctx_called;
static mctx_t       *mctx_creat;
static void         (*mctx_creat_func)(void*);
static void         *mctx_creat_arg;
static sigset_t     mctx_creat_sigs;

/**
 * source: https://dl.acm.org/citation.cfm?id=1267744
 * Original Author: Ralf S. Engelschall
 */
void mctx_create_boot(void) {
    void (*mctx_start_func)(void *);
    void *mctx_start_arg;

    /* Step 10: */
    sigprocmask(SIG_SETMASK, &mctx_creat_sigs, NULL);

    /* Step 11: */
    mctx_start_func = mctx_creat_func;
    mctx_start_arg = mctx_creat_arg;

    /* Step 12 and Step 13: */
    mctx_switch(mctx_creat, &mctx_caller);

    /* The thread "magically" starts... */
    mctx_start_func(mctx_start_arg);

    /* NOTREACHED */
    abort();
}

/**
 * source: https://dl.acm.org/citation.cfm?id=1267744
 * Original Author: Ralf S. Engelschall
 */
void mctx_create_trampoline(int sig) {
    /* Step 5: */
    if (mctx_save(mctx_creat) == 0) {
        mctx_called = true;
        return;
    }
    /* Step 9: */
    mctx_create_boot();
}

/**
 * mctx_create(): creates a machine context for a thread.
 * source: https://dl.acm.org/citation.cfm?id=1267744
 * Original Author: Ralf S. Engelschall
 */
void mctx_create(mctx_t *mctx, void (*sf_addr)(void *), void *sf_arg, void *sk_addr, size_t sk_size) {
    struct sigaction sa;
    struct sigaction osa;
    stack_t ss;
    stack_t oss;
    sigset_t osigs;
    sigset_t sigs;

    /* Step 1: */
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGUSR1);
    sigprocmask(SIG_BLOCK, &sigs, &osigs);

    /* Step 2: */
    memset((void *) &sa, 0, sizeof (struct sigaction));
    sa.sa_handler = mctx_create_trampoline;
    sa.sa_flags = SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, &osa);

    /* Step 3: */
    ss.ss_sp = sk_addr;
    ss.ss_size = sk_size;
    ss.ss_flags = 0;
    sigaltstack(&ss, &oss);

    /* Step 4: */
    mctx_creat = mctx;
    mctx_creat_func = sf_addr;
    mctx_creat_arg = sf_arg;
    mctx_creat_sigs = osigs;
    mctx_called = false;
    kill(getpid(), SIGUSR1);
    sigfillset(&sigs);
    sigdelset(&sigs, SIGUSR1);

    while (!mctx_called) {
        sigsuspend(&sigs);
    }

    /* Step 6: */
    sigaltstack(NULL, &ss);
    ss.ss_flags = SS_DISABLE;
    sigaltstack(&ss, NULL);

    if (!(oss.ss_flags & SS_DISABLE))
        sigaltstack(&oss, NULL);

    sigaction(SIGUSR1, &osa, NULL);
    sigprocmask(SIG_SETMASK, &osigs, NULL);

    /* Step 7 and Step 8: */
    mctx_switch(&mctx_caller, mctx);

    /* Step 14: */
    return;
}

/**
 * my_thr_create initializes a thread to start at func with arg func_arg,
 * this created thread is then scheduled to run in the uc array
 */
void my_thr_create(int thr_id, void (*func) (void*), void* func_arg) {
    num_threads++;
    active_threads++;

    // dynamically allocate thread contexts (aka uc) and stack space using realloc
    if (num_threads >= max_threads) {
        max_threads = (max_threads + 1) * 2;
        uc = realloc(uc, max_threads * sizeof(*uc));
        if (uc == NULL) {
            fprintf(stderr, "uc malloc/realloc failed\n");
        }

        // allocate stack pointers to accompany the new thread contexts made
        stacks = realloc(stacks, max_threads * sizeof(stacks));
        if (stacks == NULL) {
            fprintf(stderr, "stacks malloc/realloc failed\n");
        }
    }

    // initialize the context for this thread
    stacks[thr_id] = malloc(STACK_SIZE);
    mctx_create(&uc[thr_id], func, func_arg, stacks[thr_id], STACK_SIZE);
}

// starts the first thread scheduled in uc
void my_thr_start() {
    if (DEBUG) {
        printf("scheduling first thread of uc!\n");
    }
    // start the thread scheduled at uc[0], and return here when done
    if (setjmp(end) == 0) {
        mctx_switch(&main_thread, &uc[0]);
    }
    // first thread has returned, undergo exit routine

    if (DEBUG) {
        printf("Thread has finished, now calling exit routing\n");
    }
    _exit_thread();
}

void my_thr_exit() {
    longjmp(end, 1);
}

// rewrites the current thread in the uc array
// schedules the next thread in line.
// if active threads is 0, returns to the main program
void _exit_thread() {
    active_threads--;
    free(stacks[0]);
    // delete the current context from the running queue
    for (int i = 0; i < active_threads; i++) {
        uc[i] = uc[i+1];
        stacks[i] = stacks[i+1];
    }
    if (active_threads > 0) {
        my_thr_start(); // set the next scheduled thread in motion
    } else { // else, frees all the threads and returns control to program
        free(uc);
        free(stacks);
    }
}

// pauses execution of current thread and cooperatively yields
// to next scheduled thread. current thread gets put at end of queue (uc).
// if there is only 1 active thread, my_thr_yield does not do anything
void my_thr_yield() {
    if (active_threads <= 1) {
        return;
    }

    // save the context of the current thread in these temp vars
    mctx_t current_thread = uc[0];
    char* current_stack = stacks[0];

    // schedule the current thread at the end of the queue
    // do the same for the stack pointers
    for (int i = 0; i < active_threads - 1; i++) {
        uc[i] = uc[i+1];
        stacks[i] = stacks[i+1];
    }
    uc[active_threads - 1] = current_thread;
    stacks[active_threads - 1] = current_stack;

    // rewrite the old thread's context so it can resume later.
    // start the next scheduled thread
    mctx_switch(&uc[active_threads - 1], &uc[0]);
}