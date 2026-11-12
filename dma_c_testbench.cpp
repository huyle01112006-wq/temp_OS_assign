/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 *
 * demacOprx release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 *
 * DMA co-processor request queue and standalone testbench
 */

#ifndef DMA_MAIN
#define DMA_MAIN 1
#endif

#include "sysbus.h"
#include "dpi_rtl.h"
#include "dma.h"
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>

#include <verilated.h>
#include <verilated_vcd_c.h>
#include "Vdma_top.h"
#include "Vdma_top__Dpi.h"

#if DMA_MAIN == 1
extern "C" {
#include "common.h"
#include "os-mm.h"
}
#include <pthread.h>
#endif

extern "C" int dpi_rtl_dma_copy(uint64_t src_offset, uint64_t dst_offset, uint64_t length);

namespace {
struct dma_request {
    uint64_t source;
    uint64_t destination;
    uint64_t length;
};

std::mutex dma_mutex;
std::condition_variable dma_condition;
std::deque<dma_request> dma_requests;
bool dma_ready = false;
bool dma_stopping = false;
bool dma_busy = false;
int dma_initialization_status = -1;
std::atomic<bool> dma_completion_received(false);
}

extern "C" int do_dma(struct pcb_t *caller, uint64_t source,
                      uint64_t destination, uint64_t length) {
    (void)caller;
    if (length == 0) {
        return -1;
    }

    const size_t bank_size = memory_bank_get_size();
    if (!memory_bank_is_available() ||
        source > bank_size || length > bank_size - source ||
        destination > bank_size || length > bank_size - destination) {
        return -1;
    }

    std::lock_guard<std::mutex> lock(dma_mutex);
    if (!dma_ready || dma_initialization_status != 0 || dma_stopping) {
        return -1;
    }

    dma_requests.push_back({source, destination, length});
    dma_condition.notify_one();
    return 0;
}

extern "C" void dma_engine_run(int initialization_status) {
    std::unique_lock<std::mutex> lock(dma_mutex);
    dma_initialization_status = initialization_status;
    dma_ready = true;
    dma_condition.notify_all();
    if (initialization_status != 0) {
        return;
    }

    while (true) {
        dma_condition.wait(lock, [] {
            return dma_stopping || !dma_requests.empty();
        });
        if (dma_requests.empty() && dma_stopping) {
            break;
        }

        const dma_request request = dma_requests.front();
        dma_requests.pop_front();
        dma_busy = true;
        lock.unlock();

        dma_completion_received.store(false);
        const int result = dpi_rtl_dma_copy(
            request.source, request.destination, request.length);
        if (result != 0 || !dma_completion_received.load()) {
            std::fprintf(stderr,
                "DMA transfer failed: src=%llu dst=%llu len=%llu\n",
                static_cast<unsigned long long>(request.source),
                static_cast<unsigned long long>(request.destination),
                static_cast<unsigned long long>(request.length));
        }

        lock.lock();
        dma_busy = false;
        dma_condition.notify_all();
    }
}

extern "C" int dma_engine_wait_ready(void) {
    std::unique_lock<std::mutex> lock(dma_mutex);
    dma_condition.wait(lock, [] { return dma_ready; });
    return dma_initialization_status;
}

extern "C" int dma_engine_wait_idle(void) {
    std::unique_lock<std::mutex> lock(dma_mutex);
    dma_condition.wait(lock, [] { return dma_requests.empty() && !dma_busy; });
    return 0;
}

extern "C" void dma_engine_request_stop(void) {
    std::lock_guard<std::mutex> lock(dma_mutex);
    dma_stopping = true;
    dma_condition.notify_all();
}

extern "C" void dma_engine_notify_completion(void) {
    dma_completion_received.store(true);
}

#if DMA_MAIN == 1
static void * standalone_co_processor(void *) {
    const int status = rtl_dma_init_simulation();
    if (status != 0) {
        std::fprintf(stderr, "Cannot initialize RTL simulation\n");
    }
    dma_engine_run(status);
    rtl_dma_cleanup_simulation();
    return NULL;
}

int main(int argc, char **argv) {
    uint64_t source = 0x1000;
    uint64_t destination = 0x2000;
    uint64_t length = 512;
    if (argc >= 4) {
        source = strtoull(argv[1], NULL, 0);
        destination = strtoull(argv[2], NULL, 0);
        length = strtoull(argv[3], NULL, 0);
    }

    std::printf(
        "Running dma_testbench source=0x%llx, destination=0x%llx, length=%llu\n",
        static_cast<unsigned long long>(source),
        static_cast<unsigned long long>(destination),
        static_cast<unsigned long long>(length));

    if (memory_bank_init(1024 * 1024) == MEMORY_BANK_INVALID) {
        std::fprintf(stderr, "Cannot initialize DMA memory bank\n");
        return 1;
    }
    const size_t bank_size = memory_bank_get_size();
    if (source > bank_size || length > bank_size - source ||
        destination > bank_size || length > bank_size - destination ||
        length == 0) {
        std::fprintf(stderr, "DMA test range is outside the memory bank\n");
        memory_bank_cleanup();
        return 1;
    }

    std::uint8_t *source_data =
        static_cast<std::uint8_t *>(memory_bank_get_ptr(source));
    std::uint8_t *destination_data =
        static_cast<std::uint8_t *>(memory_bank_get_ptr(destination));
    for (uint64_t i = 0; i < length; ++i) {
        source_data[i] = static_cast<std::uint8_t>((i % 255) + 1);
    }

    pthread_t engine;
    if (pthread_create(&engine, NULL, standalone_co_processor, NULL) != 0) {
        memory_bank_cleanup();
        std::fprintf(stderr, "Cannot start DMA co-processor thread\n");
        return 1;
    }
    if (dma_engine_wait_ready() != 0) {
        dma_engine_request_stop();
        pthread_join(engine, NULL);
        memory_bank_cleanup();
        return 1;
    }

    int result = do_dma(NULL, source, destination, length);
    if (result == 0) {
        dma_engine_wait_idle();
        for (uint64_t i = 0; i < length; ++i) {
            if (source_data[i] != destination_data[i]) {
                std::fprintf(stderr, "DMA data mismatch at byte %llu\n",
                    static_cast<unsigned long long>(i));
                result = -1;
                break;
            }
        }
    }
    dma_engine_request_stop();
    pthread_join(engine, NULL);
    memory_bank_cleanup();
    return result == 0 ? 0 : 1;
}
#endif
