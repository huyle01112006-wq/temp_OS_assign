#include <stdio.h>
#include "queue.h"

int empty(struct queue_t *q)
{
	return q == NULL || q->size == 0;
}

void enqueue(struct queue_t *q, struct pcb_t *proc)
{
#ifndef MLQ_SCHED
	int i;
#endif
	if (q == NULL || proc == NULL) {
		return;
	}
	if (q->size == MAX_QUEUE_SIZE) {
		fprintf(stderr, "scheduler queue is full\n");
		return;
	}
	q->proc[q->size++] = proc;
#ifndef MLQ_SCHED
	for (i = q->size - 1; i > 0 &&
	     q->proc[i]->priority < q->proc[i - 1]->priority; --i) {
		struct pcb_t *tmp = q->proc[i];
		q->proc[i] = q->proc[i - 1];
		q->proc[i - 1] = tmp;
	}
#endif
}

struct pcb_t *dequeue(struct queue_t *q)
{
	struct pcb_t *proc;
	int i;
	if (empty(q)) {
		return NULL;
	}
	proc = q->proc[0];
	for (i = 1; i < q->size; ++i) {
		q->proc[i - 1] = q->proc[i];
	}
	q->proc[--q->size] = NULL;
	return proc;
}

struct pcb_t *purgequeue(struct queue_t *q, struct pcb_t *proc)
{
	int i;
	if (q == NULL || proc == NULL) {
		return NULL;
	}
	for (i = 0; i < q->size; ++i) {
		if (q->proc[i] == proc) {
			int j;
			for (j = i + 1; j < q->size; ++j) {
				q->proc[j - 1] = q->proc[j];
			}
			q->proc[--q->size] = NULL;
			return proc;
		}
	}
	return NULL;
}
