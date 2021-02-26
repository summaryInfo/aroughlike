/* Copyright (c) 2021, Evgeny Baskov. All rights reserved */

#define _POSIX_C_SOURCE 200809L

#include "worker.h"

#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#define MAX_THREADS 16

pthread_t threads[MAX_THREADS];
size_t nproc;


struct job {
    struct job *next;
    void (*func)(void *);
    char data[];
};

static pthread_cond_t in_cond;
static pthread_mutex_t in_mtx;
struct job *first, *last;
static _Bool should_exit;

static size_t active;

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

        free(newjob);
    }
    return NULL;
}

void submit_work(void (*func)(void *), void *data, size_t data_size) {
    if (func) {
        struct job *new = calloc(1, sizeof(*new) + data_size);
        new->func = func;
        memcpy(new->data, data, data_size);

        pthread_mutex_lock(&in_mtx);
        if (last) {
            last->next = new;
            last = new;
        } else last = first = new;

        pthread_mutex_unlock(&in_mtx);
    }

    //pthread_cond_signal(&in_cond);
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
            free(newjob);
        }

        pthread_mutex_lock(&in_mtx);
    }
    pthread_mutex_unlock(&in_mtx);
}

void init_workers(void) {
    pthread_cond_init(&in_cond, NULL);
    pthread_mutex_init(&in_mtx, NULL);

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
}
