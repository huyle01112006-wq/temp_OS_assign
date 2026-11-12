/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 *
 * demacOprx release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 *
 * System OS module
 */
#include "cpu.h"
#include "timer.h"
#include "sched.h"
#include "loader.h"
#include "mm.h"
#ifdef MM64
#include "mm64.h"
#endif

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>

#include "sysbus.h"
#include "dpi_rtl.h"
#include "dma.h"

static int time_slot;
static int num_cpus;
static atomic_int done = 0;
static struct krnl_t os;

#ifdef MM_PAGING
static unsigned long memramsz;
static unsigned long memswpsz[PAGING_MAX_MMSWP];

struct mmpaging_ld_args {
	/* A dispatched argument struct to compact many-fields passing to loader */
	int vmemsz;
	struct memphy_struct *mram;
	struct memphy_struct **mswp;
	struct memphy_struct *active_mswp;
	int active_mswp_id;
	struct timer_id_t  *timer_id;
};
#endif

static struct ld_args{
	char ** path;
	unsigned long * start_time;
#ifdef MLQ_SCHED
	unsigned long * prio;
#endif
} ld_processes;
int num_processes;

struct cpu_args {
	struct timer_id_t * timer_id;
	int id;
};

static void * cpu_routine(void * args) {
	struct timer_id_t * timer_id = ((struct cpu_args*)args)->timer_id;
	int id = ((struct cpu_args*)args)->id;
	/* Check for new process in ready queue */
	int time_left = 0;
	struct pcb_t * proc = NULL;
	while (1) {
		/* Check the status of current process */
		if (proc == NULL) {
			/* No process is running, the we load new process from
		 	* ready queue */
			proc = get_proc();
			if (proc == NULL && !done) {
                           next_slot(timer_id);
                           continue; /* First load failed. skip dummy load */
                        }
		}else if (proc->pc == proc->code->size) {
			/* The porcess has finish it job */
			printf("\tCPU %d: Processed %2d has finished\n",
				id ,proc->pid);
			{
				struct mm_struct *user_mm =
					mm_get_process(proc->krnl, proc->pid, 0);
				struct mm_struct *kernel_mm =
					mm_get_process(proc->krnl, proc->pid, 1);
				mm_print_stats(user_mm, proc->pid, "user");
				mm_print_stats(kernel_mm, proc->pid, "kernel");
				mm_unregister_process(proc->krnl, proc->pid);
				mm_destroy(user_mm, proc->krnl->mram);
				mm_destroy(kernel_mm, proc->krnl->mram);
			}
			free(proc->code->text);
			free(proc->code);
			free(proc);
			proc = get_proc();
			time_left = 0;
		}else if (time_left == 0) {
			/* The process has done its job in current time slot */
			printf("\tCPU %d: Put process %2d to run queue\n",
				id, proc->pid);
			put_proc(proc);
			proc = get_proc();
		}
		
		/* Recheck process status after loading new process */
		if (proc == NULL && done) {
			/* No process to run, exit */
			printf("\tCPU %d stopped\n", id);
			break;
		}else if (proc == NULL) {
			/* There may be new processes to run in
			 * next time slots, just skip current slot */
			next_slot(timer_id);
			continue;
		}else if (time_left == 0) {
			printf("\tCPU %d: Dispatched process %2d\n",
				id, proc->pid);
			time_left = time_slot;
		}
		
		/* Run current process */
		run(proc);
		time_left--;
		next_slot(timer_id);
	}
	detach_event(timer_id);
	pthread_exit(NULL);
}


static void * co_processor(void *arg) {
    (void)arg;
    int status = 0;
    int owns_memory_bank = 0;
    if (!memory_bank_is_available()) {
        if (memory_bank_init(1024 * 1024) == MEMORY_BANK_INVALID) {
            status = -1;
        } else {
            owns_memory_bank = 1;
        }
    }
    if (status == 0) {
        status = rtl_dma_init_simulation();
    }
    if (status != 0) {
        fprintf(stderr, "Cannot initialize DMA co-processor\n");
    }

    dma_engine_run(status);
    rtl_dma_cleanup_simulation();
    if (owns_memory_bank) {
        memory_bank_cleanup();
    }
    return NULL;
}

static void * ld_routine(void * args) {
#ifdef MM_PAGING
	struct memphy_struct* mram = ((struct mmpaging_ld_args *)args)->mram;
	struct memphy_struct** mswp = ((struct mmpaging_ld_args *)args)->mswp;
	struct memphy_struct* active_mswp = ((struct mmpaging_ld_args *)args)->active_mswp;
	struct timer_id_t * timer_id = ((struct mmpaging_ld_args *)args)->timer_id;
#else
	struct timer_id_t * timer_id = (struct timer_id_t*)args;
#endif
	int i = 0;
	i=0;
	printf("ld_routine\n");
	os.mram = mram;
	os.mswp = mswp;
	os.active_mswp = active_mswp;
	os.process_mms = NULL;
	os.mm = (struct mm_struct*)calloc(1, sizeof(struct mm_struct));
	if (os.mm == NULL || init_mm(os.mm, NULL) != 0) {
		fprintf(stderr, "Cannot initialize kernel address space\n");
		done = 1;
		detach_event(timer_id);
		pthread_exit(NULL);
	}
	while (i < num_processes) {
		struct pcb_t * proc = load(ld_processes.path[i]);
		proc->krnl = &os;

#ifdef MLQ_SCHED
	proc->prio = ld_processes.prio[i];
#endif
	while (current_time() < ld_processes.start_time[i]) {
		next_slot(timer_id);
	}
#ifdef MM_PAGING
	{
		struct mm_struct *user_mm =
			(struct mm_struct*)calloc(1, sizeof(struct mm_struct));
		struct mm_struct *kernel_mm =
			(struct mm_struct*)calloc(1, sizeof(struct mm_struct));
		if (user_mm == NULL || kernel_mm == NULL ||
		    init_mm(user_mm, proc) != 0 ||
		    init_mm(kernel_mm, proc) != 0 ||
		    mm_register_process(&os, proc->pid, user_mm, kernel_mm) != 0) {
			fprintf(stderr, "Cannot initialize address spaces for PID %u\n",
			        proc->pid);
			mm_destroy(user_mm, os.mram);
			mm_destroy(kernel_mm, os.mram);
			free(proc->code->text);
			free(proc->code);
			free(proc);
			done = 1;
			detach_event(timer_id);
			pthread_exit(NULL);
		}
	}
#endif
		printf("\tLoaded a process at %s, PID: %d PRIO: %ld\n",
			ld_processes.path[i], proc->pid, ld_processes.prio[i]);
		add_proc(proc);
		free(ld_processes.path[i]);
		i++;
		next_slot(timer_id);
	}
	free(ld_processes.path);
	free(ld_processes.start_time);
	done = 1;
	detach_event(timer_id);
	pthread_exit(NULL);
}

static void read_config(const char * path) {
	FILE * file;
	if ((file = fopen(path, "r")) == NULL) {
		printf("Cannot find configure file at %s\n", path);
		exit(1);
	}
	fscanf(file, "%d %d %d\n", &time_slot, &num_cpus, &num_processes);
	ld_processes.path = (char**)malloc(sizeof(char*) * num_processes);
	ld_processes.start_time = (unsigned long*)
		malloc(sizeof(unsigned long) * num_processes);
#ifdef MM_PAGING
	int sit;
#ifdef MM_FIXED_MEMSZ
	/* We provide here a back compatible with legacy OS simulatiom config file
         * In which, it have no addition config line for Mema, keep only one line
	 * for legacy info 
         *  [time slice] [N = Number of CPU] [M = Number of Processes to be run]
         */
        memramsz  =  0x100000000;
        memswpsz[0] = 0x1000000;
	for(sit = 1; sit < PAGING_MAX_MMSWP; sit++)
		memswpsz[sit] = 0;
#else
	/* Read input config of memory size: MEMRAM and upto 4 MEMSWP (mem swap)
	 * Format: (size=0 result non-used memswap, must have RAM and at least 1 SWAP)
	 *        MEM_RAM_SZ MEM_SWP0_SZ MEM_SWP1_SZ MEM_SWP2_SZ MEM_SWP3_SZ
	*/
	fscanf(file, FORMAT_ARG "\n", &memramsz);
	for(sit = 0; sit < PAGING_MAX_MMSWP; sit++)
		fscanf(file, FORMAT_ARG, &(memswpsz[sit])); 

       fscanf(file, "\n"); /* Final character */
#endif
#endif

#ifdef MLQ_SCHED
	ld_processes.prio = (unsigned long*)
		malloc(sizeof(unsigned long) * num_processes);
#endif
	int i;
	for (i = 0; i < num_processes; i++) {
		ld_processes.path[i] = (char*)malloc(sizeof(char) * 100);
		ld_processes.path[i][0] = '\0';
		strcat(ld_processes.path[i], "input/proc/");
		char proc[100];
#ifdef MLQ_SCHED
		fscanf(file, "%lu %s %lu\n", &ld_processes.start_time[i], proc, &ld_processes.prio[i]);
		if (ld_processes.prio[i] >= MAX_PRIO) {
			fprintf(stderr, "Invalid MLQ priority %lu for process %s\n",
			        ld_processes.prio[i], proc);
			exit(1);
		}
#else
		fscanf(file, "%lu %s\n", &ld_processes.start_time[i], proc);
#endif
		strcat(ld_processes.path[i], proc);
	}
}

int main(int argc, char * argv[]) {
	/* Read config */
	if (argc != 2) {
		printf("Usage: os [path to configure file]\n");
		return 1;
	}
	char path[100];
	path[0] = '\0';
	strcat(path, "input/");
	strcat(path, argv[1]);
	read_config(path);

	pthread_t * cpu = (pthread_t*)malloc(num_cpus * sizeof(pthread_t));
	pthread_t * dma = (pthread_t*)malloc(sizeof(pthread_t));
	struct cpu_args * args =
		(struct cpu_args*)malloc(sizeof(struct cpu_args) * num_cpus);
	pthread_t ld;
	
	/* Init timer */
	int i;
	for (i = 0; i < num_cpus; i++) {
		args[i].timer_id = attach_event();
		args[i].id = i;
	}
	struct timer_id_t * ld_event = attach_event();

#ifdef MM_PAGING
	/* Init all MEMPHY include 1 MEMRAM and n of MEMSWP */
	int rdmflag = 1; /* By default memphy is RANDOM ACCESS MEMORY */

	struct memphy_struct mram;
	struct memphy_struct mswp[PAGING_MAX_MMSWP];
	memset(&mram, 0, sizeof(mram));
	memset(mswp, 0, sizeof(mswp));

	/* Create MEM RAM */
	if (init_memphy(&mram, memramsz, rdmflag) != 0) {
		fprintf(stderr, "Cannot initialize main memory (%lu bytes)\n", memramsz);
		free(dma);
		free(cpu);
		free(args);
		return 1;
	}

        /* Create all MEM SWAP */ 
	int sit;
	for(sit = 0; sit < PAGING_MAX_MMSWP; sit++) {
		if (memswpsz[sit] != 0 &&
		    init_memphy(&mswp[sit], memswpsz[sit], rdmflag) != 0) {
			fprintf(stderr, "Cannot initialize swap space %d (%lu bytes)\n",
			        sit, memswpsz[sit]);
			destroy_memphy(&mram);
			while (--sit >= 0) {
				destroy_memphy(&mswp[sit]);
			}
			free(dma);
			free(cpu);
			free(args);
			return 1;
		}
	}

	/* In Paging mode, it needs passing the system mem to each PCB through loader*/
	struct mmpaging_ld_args *mm_ld_args = (struct mmpaging_ld_args*)malloc(sizeof(struct mmpaging_ld_args));
	if (mm_ld_args == NULL) {
		fprintf(stderr, "Cannot allocate loader arguments\n");
		destroy_memphy(&mram);
		for (sit = 0; sit < PAGING_MAX_MMSWP; ++sit) {
			destroy_memphy(&mswp[sit]);
		}
		free(dma);
		free(cpu);
		free(args);
		return 1;
	}

	mm_ld_args->timer_id = ld_event;
	mm_ld_args->mram = (struct memphy_struct *) &mram;
	mm_ld_args->mswp = (struct memphy_struct**) &mswp;
	mm_ld_args->active_mswp = (struct memphy_struct *) &mswp[0];
        mm_ld_args->active_mswp_id = 0;


#endif

	/* Init scheduler */
	init_scheduler();

	/* Initialize the Verilator-backed engine before CPUs can submit DMA work. */
	if (pthread_create(dma, NULL, co_processor, NULL) != 0) {
		fprintf(stderr, "Cannot start DMA co-processor thread\n");
		free(dma);
		free(mm_ld_args);
		destroy_memphy(&mram);
		for (i = 0; i < PAGING_MAX_MMSWP; ++i) {
			destroy_memphy(&mswp[i]);
		}
		free(cpu);
		free(args);
		return 1;
	}
	if (dma_engine_wait_ready() != 0) {
		pthread_join(*dma, NULL);
		free(dma);
		free(mm_ld_args);
		destroy_memphy(&mram);
		for (i = 0; i < PAGING_MAX_MMSWP; ++i) {
			destroy_memphy(&mswp[i]);
		}
		free(cpu);
		free(args);
		return 1;
	}
	start_timer();

	/* Run CPU and loader */
#ifdef MM_PAGING
	pthread_create(&ld, NULL, ld_routine, (void*)mm_ld_args);
#else
	pthread_create(&ld, NULL, ld_routine, (void*)ld_event);
#endif
	for (i = 0; i < num_cpus; i++) {
		pthread_create(&cpu[i], NULL,
			cpu_routine, (void*)&args[i]);
	}

	/* Wait for CPU and loader finishing */
	for (i = 0; i < num_cpus; i++) {
		pthread_join(cpu[i], NULL);
	}
	pthread_join(ld, NULL);

	/* Drain outstanding DMA requests before stopping the co-processor. */
	dma_engine_request_stop();
	pthread_join(*dma, NULL);
	free(dma);

	/* Stop timer */
	stop_timer();
	destroy_memphy(os.mram);
	for (i = 0; i < PAGING_MAX_MMSWP; i++) {
		destroy_memphy(&mswp[i]);
	}

	return 0;

}
