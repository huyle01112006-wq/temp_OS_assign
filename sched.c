/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 */

#include "queue.h"
#include "sched.h"
#include <pthread.h>
#include <string.h>

static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
#ifdef MLQ_SCHED
static struct queue_t mlq_ready_queue[MAX_PRIO];
#else
static struct queue_t ready_queue;
static struct queue_t run_queue;
#endif

int queue_empty(void)
{
	int i;
	int result = 1;
	pthread_mutex_lock(&queue_lock);
#ifdef MLQ_SCHED
	for (i = 0; i < MAX_PRIO; ++i) {
		if (!empty(&mlq_ready_queue[i])) {
			result = 0;
			break;
		}
	}
#else
	result = empty(&ready_queue) && empty(&run_queue);
#endif
	pthread_mutex_unlock(&queue_lock);
	return result;
}

void init_scheduler(void)
{
	pthread_mutex_lock(&queue_lock);
#ifdef MLQ_SCHED
	memset(mlq_ready_queue, 0, sizeof(mlq_ready_queue));
#else
	memset(&ready_queue, 0, sizeof(ready_queue));
	memset(&run_queue, 0, sizeof(run_queue));
#endif
	pthread_mutex_unlock(&queue_lock);
}

struct pcb_t *get_proc(void)
{
	struct pcb_t *proc = NULL;
	pthread_mutex_lock(&queue_lock);
#ifdef MLQ_SCHED
	int prio;
	for (prio = 0; prio < MAX_PRIO; ++prio) {
		if (!empty(&mlq_ready_queue[prio])) {
			proc = dequeue(&mlq_ready_queue[prio]);
			break;
		}
	}
#else
	proc = empty(&run_queue) ? dequeue(&ready_queue) : dequeue(&run_queue);
#endif
	pthread_mutex_unlock(&queue_lock);
	return proc;
}

void put_proc(struct pcb_t *proc)
{
	if (proc == NULL) {
		return;
	}
	pthread_mutex_lock(&queue_lock);
#ifdef MLQ_SCHED
	if (proc->prio >= MAX_PRIO) {
		pthread_mutex_unlock(&queue_lock);
		return;
	}
	enqueue(&mlq_ready_queue[proc->prio], proc);
#else
	enqueue(&run_queue, proc);
#endif
	pthread_mutex_unlock(&queue_lock);
}

void add_proc(struct pcb_t *proc)
{
	if (proc == NULL) {
		return;
	}
	pthread_mutex_lock(&queue_lock);
#ifdef MLQ_SCHED
	if (proc->prio >= MAX_PRIO) {
		pthread_mutex_unlock(&queue_lock);
		return;
	}
	enqueue(&mlq_ready_queue[proc->prio], proc);
#else
	enqueue(&ready_queue, proc);
#endif
	pthread_mutex_unlock(&queue_lock);
}

void finish_scheduler(void)
{
}
