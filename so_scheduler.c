#include "so_scheduler.h"
#include "utils.h"
#include <stdio.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdlib.h>

#define MAXSIZE 1024
#define TRUE 1
#define FALSE 0

typedef unsigned int uint32_t;
typedef unsigned char uint8_t;
typedef enum {NEW, READY, RUNNING, WAITING, TERMINATED} thr_status;

typedef struct {
	tid_t tid;
	uint8_t priority;
	uint8_t time;
	thr_status status;

    uint32_t device;
    so_handler *handler;
	sem_t thread_sem;
} thread;

typedef struct {
	int quantum;
    uint8_t initialized;
	uint32_t io_events;

	thread *running_thread;

	thread *threads[MAXSIZE];
    int threads_no;

	thread *prio_queue[MAXSIZE];
    int queue_size;
} scheduler;

static void push(thread *thread);
static void pop(void);

static int init_thread(thread *new_thread, so_handler *func, uint32_t priority);
static void set_thread_status(thread *thread, thr_status status);
static void thread_start(void *args);
static uint8_t find_next_thread(void);
static void run_next_thread(void);


static int8_t check_init(uint32_t time_quantum, uint32_t io);
static int8_t check_wait(uint32_t io_events);
static int8_t check_fork(so_handler *func, uint32_t priority);
static int8_t check_signal(uint32_t io);
static int8_t check_device_status(thread *current_thread, uint32_t io);

static void call_scheduler(void);
static void scheduler_init(uint32_t time_quantum, uint32_t io);

static int8_t blocked_or_terminated(thread *current_thread);
static int8_t higher_priority(thread *current_thread, thread *next_thread);
static int8_t same_priority_time_up(thread *current_thread, thread *next_thread);


static void join_threads(void);
static void destroy_threads(void);
static void destroy_queue(void);
static void destroy_scheduler(void);

static scheduler my_scheduler;

int so_init(uint32_t time_quantum, uint32_t io)
{
    /*
    Check if the scheduler is already initialized and
    if the given args are valid.
    */

	if (check_init(time_quantum, io) < 0)
		return -1;

    /*
    Initialize the scheduler.
    */

    scheduler_init(time_quantum, io);

	return 0;
}

void so_end(void)
{
    /*
    If the scheduler is not initialized, simply return.
    Otherwise, proceed with the destruction of the scheduler.
    (join threads, free threads array, free queue,
    set initialized flag to 0).
    */

	if (!my_scheduler.initialized)
		return;

    destroy_scheduler();
}

void so_exec(void)
{
    /*
    There is a thread running, decrement its time.
    Call scheduler, so as to check if another thread
    should be started.
    */

	thread *current_thread = my_scheduler.running_thread;
	current_thread->time--;

	call_scheduler();

	DIE(sem_wait(&current_thread->thread_sem), "thread sem wait");
}

int so_wait(uint32_t io)
{
	int ret_err;

    /*
    Check if the given args are valid.
    */

	if (check_wait(io) < 0) {
        return -1;
    }
	
    /*
    Set the device and status of the running thread.
    */

    my_scheduler.running_thread->device = io;
	set_thread_status(my_scheduler.running_thread, WAITING);
    call_scheduler();

	ret_err = sem_wait(&my_scheduler.running_thread->thread_sem);
	DIE(ret_err != 0, "thread sem wait");

	return 0;
}

int so_signal(uint32_t io)
{
    /*
    Check if the given args are valid.
    */

	if (check_signal(io) < 0)
		return -1;

    uint32_t count = 0, i;

    for (i = 0; (int) i < my_scheduler.threads_no; i++) {
        if (check_device_status(my_scheduler.threads[i], io) >= 0) {
            my_scheduler.threads[i]->device = SO_MAX_NUM_EVENTS;
            set_thread_status(my_scheduler.threads[i], READY);
            push(my_scheduler.threads[i]);
            count++;
        }
    }

    so_exec();
    return count;
}

tid_t so_fork(so_handler *func, uint32_t priority)
{
    /*
    Check if the given args are valid.
    */

	if (check_fork(func, priority) < 0)
        return INVALID_TID;

    /*
    Allocate memory for the new thread and initialize it.
    */

    thread *new_thread = malloc(sizeof(thread));
	DIE(init_thread(new_thread, func, priority), "new thread init / create");

    /*
    Add the new thread to the priority queue.
    */

	push(new_thread);

    /*
    Add the new thread to the threads array.
    */

	my_scheduler.threads[my_scheduler.threads_no++] = new_thread;

    /*
    If there is a running thread, decrement its time
    and check if preemption is needed. Otherwise,
    run the next thread (try finding a suitable one). 
    */

	if (my_scheduler.running_thread) {
        so_exec();
    }
    else {
        run_next_thread();
    }
        
	return new_thread->tid;
}

static void thread_start(void *args)
{
	DIE(sem_wait(&((thread *)args)->thread_sem), "sem wait");

    ((thread *)args)->handler(((thread *)args)->priority);
    set_thread_status((thread *)args, TERMINATED);

	call_scheduler();
}

static void reset_quantum(thread *current_thread)
{
    if (current_thread->time < 1)
        current_thread->time = my_scheduler.quantum;
}

static int8_t check_scheduler(thread *current_thread) {

    if (current_thread && my_scheduler.queue_size == 0)
        return 0;

    return -1;
}

static void call_scheduler(void)
{
	int ret_err;
	thread *current_thread = my_scheduler.running_thread;  

    /*
    Check if the running thread is the only one left. If so,
    reset its quantum and return. Otherwise, proceed with
    finding the next thread (done with find_next_thread), which
    checks for 3 cases:

    1. The next thread is either waiting or terminated.
    2. The next thread has a higher priority.
    3. The next thread has an equal priority, but out of running time.
    */

	if (check_scheduler(current_thread) >= 0 || !find_next_thread()) {

        reset_quantum(current_thread);
		ret_err = sem_post(&current_thread->thread_sem);
		DIE(ret_err != 0, "thread sem post");
	}
}

static int8_t blocked_or_terminated(thread *current_thread)
{
    if (current_thread->status == WAITING || current_thread->status == TERMINATED) {
        run_next_thread();
        return 0;
    }

    return -1;
}

static int8_t higher_priority(thread *current_thread, thread *next_thread)
{
    if (next_thread->priority > current_thread->priority) {
		run_next_thread();
		push(current_thread);
		return 0;
	}

    return -1;
}

static int8_t same_priority_time_up(thread *current_thread, thread *next_thread)
{
    if (current_thread->time <= 0) {
		if (current_thread->priority == next_thread->priority) {
			run_next_thread();
			push(current_thread);
			return 0;
		}
    }

    return -1;
}

static uint8_t find_next_thread(void)
{
	thread *next_thread, *current_thread;

	current_thread = my_scheduler.running_thread;
	next_thread = my_scheduler.prio_queue[0];
	
	if (blocked_or_terminated(current_thread) >= 0 
    || higher_priority(current_thread, next_thread) >= 0
    || same_priority_time_up(current_thread, next_thread) >= 0) {
        return TRUE;
    }
        
	return FALSE;
}

static void run_next_thread(void)
{
    /*
    Set the running thread to the first thread in the queue.
    */

	my_scheduler.running_thread = my_scheduler.prio_queue[0];

    /*
    Pop the first thread from the queue.
    */

	pop();

    /*
    Set the running thread's status to RUNNING and reset its quantum.
    */

    set_thread_status(my_scheduler.running_thread, RUNNING);
    reset_quantum(my_scheduler.running_thread);
	
	DIE(sem_post(&my_scheduler.running_thread->thread_sem), "thread sem post");
}

static void pop(void)
{
	int i = 0;

	while(i < my_scheduler.queue_size - 1) {
        my_scheduler.prio_queue[i] = my_scheduler.prio_queue[i + 1];
        i++;
    }

	my_scheduler.prio_queue[my_scheduler.queue_size - 1] = NULL;
	my_scheduler.queue_size--;
}


static void push(thread *thread)
{
	int i;

    for(i = 0; i < my_scheduler.queue_size && my_scheduler.prio_queue[i]->priority >= thread->priority; i++);

    int j = i;
    i = my_scheduler.queue_size;

    if (j != my_scheduler.queue_size) {
        while (i > j) {
            my_scheduler.prio_queue[i] = my_scheduler.prio_queue[i - 1];
            i--;
        } 
    }

	my_scheduler.queue_size++;
	my_scheduler.prio_queue[j] = thread;
    set_thread_status(my_scheduler.prio_queue[j], READY);
}

static int8_t check_init(uint32_t time_quantum, uint32_t io)
{
    if (my_scheduler.initialized || time_quantum <=0 || io > SO_MAX_NUM_EVENTS) {
        return -1;
    }
    return 0;
}

static void scheduler_init(uint32_t time_quantum, uint32_t io)
{

    my_scheduler.initialized = TRUE;
	my_scheduler.io_events = io;
    my_scheduler.quantum = time_quantum;
	my_scheduler.threads_no = 0;
	my_scheduler.queue_size = 0;
	my_scheduler.running_thread = NULL;
}

static void set_thread_status(thread *thread, thr_status status)
{
    thread->status = status;
}

static int8_t check_fork(so_handler *func, uint32_t priority) 
{
    if (func == NULL || priority > SO_MAX_PRIO)
        return -1;

    return 0;
}

static int init_thread(thread *new_thread, so_handler *func, uint32_t priority)
{   
    new_thread->tid = INVALID_TID;
    set_thread_status(new_thread, NEW);
    new_thread->time = my_scheduler.quantum;
    new_thread->device = SO_MAX_NUM_EVENTS;
    new_thread->priority = priority;
    new_thread->handler = func;

    int ret_err = sem_init(&new_thread->thread_sem, 0, 0);

    if (ret_err)
        return -1;

    ret_err = pthread_create(&new_thread->tid, NULL, (void *)thread_start,(void *)new_thread);
    
     if (ret_err)
        return -1;

    return 0;

}

static int8_t check_wait(uint32_t io_events) {
    
    if ((int)io_events < 0 || io_events >= my_scheduler.io_events)
        return -1;

    return 0;
}

static void join_threads(void) {

    for(int i = 0; i < my_scheduler.threads_no; i++) {
        pthread_join(my_scheduler.threads[i]->tid, NULL);
    }
}

static void destroy_threads(void) {

    for(int i = 0; i < my_scheduler.threads_no; i++) {
        sem_destroy(&my_scheduler.threads[i]->thread_sem);
        free(my_scheduler.threads[i]);
    }
}

static void destroy_queue(void) {

    for(int i = 0; i < my_scheduler.queue_size; i++) {
        free(my_scheduler.prio_queue[i]);
    }

}

static void destroy_scheduler(void) {

    join_threads();
    destroy_threads();
    destroy_queue();

    my_scheduler.initialized = FALSE;
}

static int8_t check_signal(uint32_t io) {
    if (io >= my_scheduler.io_events || (int)io < 0)
        return -1;

    return 0;
}

static int8_t check_device_status(thread *current_thread, uint32_t io)
{
    if (current_thread->device == io && current_thread->status == NEW)
        return 0;

    return -1;
}