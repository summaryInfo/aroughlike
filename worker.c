/* Copyright (c) 2021, Evgeny Baskov. All rights reserved */

#define _POSIX_C_SOURCE 200809L

#include "worker.h"
#include "util.h"

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_THREADS 16
#define STORAGE_SIZE 65536


struct job {
    struct job *next;
    void (*func)(void *);
    char data[];
};

size_t nproc;

static pthread_t threads[MAX_THREADS];

static pthread_cond_t in_cond;
static pthread_mutex_t in_mtx;
static pthread_rwlock_t rw;
struct job *first, *last;
static _Bool should_exit;

static size_t active;
static uint8_t *storage_start;
static uint8_t *storage_cur;
static uint8_t *storage_end;

static void *worker(void *arg) {
    (void)arg;


    while (!__atomic_load_n(&should_exit, __ATOMIC_RELAXED)) {
        pthread_mutex_lock(&in_mtx);
        while (!first) {
            pthread_cond_wait(&in_cond, &in_mtx);
            if (__atomic_load_n(&should_exit, __ATOMIC_RELAXED)) {
                pthread_mutex_unlock(&in_mtx);
                return NULL;
            }
        }

        __atomic_add_fetch(&active, 1, __ATOMIC_RELEASE);

        struct job *newjob = first;
        if (first == last) last = NULL;
        first = first->next;

        pthread_mutex_unlock(&in_mtx);

        newjob->func(newjob->data);
        __atomic_sub_fetch(&active, 1, __ATOMIC_RELEASE);
    }
    return NULL;
}

void drain_work(void) {
    pthread_mutex_lock(&in_mtx);
    while (first || __atomic_load_n(&active, __ATOMIC_ACQUIRE)) {
        struct job *newjob = NULL;
        if (first) {
            __atomic_add_fetch(&active, 1, __ATOMIC_RELEASE);
            newjob = first;
            if (first == last) last = NULL;
            first = first->next;
        }

        pthread_mutex_unlock(&in_mtx);
        pthread_cond_broadcast(&in_cond);

        if (newjob) {
            newjob->func(newjob->data);
            __atomic_sub_fetch(&active, 1, __ATOMIC_RELEASE);
        }

        pthread_mutex_lock(&in_mtx);
    }

    pthread_mutex_unlock(&in_mtx);
}

void submit_work(void (*func)(void *), void *data, size_t data_size) {
    // Align args on CACHE_LINE to prefent false sharing
    size_t inc = (sizeof(struct job) + data_size + CACHE_LINE - 1) & ~(CACHE_LINE - 1);

    pthread_rwlock_rdlock(&rw);

    // Lock-less allocation
    struct job *new;
    while (1) {
        new = (void *)__atomic_fetch_add(&storage_cur, inc, __ATOMIC_ACQ_REL);
        if ((uint8_t *)new + inc <= storage_end) break;

        pthread_rwlock_unlock(&rw);
        pthread_rwlock_wrlock(&rw);
        drain_work();
        __atomic_store_n(&storage_cur, storage_start, __ATOMIC_RELEASE);
        pthread_rwlock_unlock(&rw);
        pthread_rwlock_rdlock(&rw);
    }

    new->func = func;
    new->next = NULL;
    memcpy(new->data, data, data_size);

    pthread_mutex_lock(&in_mtx);
    if (last) {
        last->next = new;
        last = new;
    } else last = first = new;
    pthread_mutex_unlock(&in_mtx);

    pthread_rwlock_unlock(&rw);

    //pthread_cond_signal(&in_cond);
}

void init_workers(void) {
    // This is used for faster
    // memory allocation for job
    // arguments
    storage_start = storage_cur = malloc(STORAGE_SIZE);
    storage_end = storage_start + STORAGE_SIZE;

    pthread_cond_init(&in_cond, NULL);
    pthread_mutex_init(&in_mtx, NULL);
    pthread_rwlock_init(&rw, NULL);

    nproc = sysconf(_SC_NPROCESSORS_ONLN) - 1;
   for (size_t i = 0; i < nproc; i++)
        pthread_create(threads + i, NULL, worker, NULL);
}

void fini_workers(_Bool force) {
    if (!force) drain_work();

    __atomic_store_n(&should_exit, 1, __ATOMIC_RELAXED);
    pthread_cond_broadcast(&in_cond);

    for (size_t i = 0; i < nproc; i++)
        pthread_join(threads[i], NULL);

    pthread_cond_destroy(&in_cond);
    pthread_mutex_destroy(&in_mtx);

    free(storage_start);
}
