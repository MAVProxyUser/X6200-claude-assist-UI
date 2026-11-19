/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  A Mac's __real_ names for the C library's functions the test catches (the Makefile): on Linux GNU ld's --wrap
 *  gives them, a Mac's linker has no --wrap. Compiled without the Makefile's renaming, so these are the real ones.
 */

#include <pthread.h>

int __real_pthread_create(pthread_t *t, const pthread_attr_t *attr, void *(*fn)(void *), void *arg);
int __real_pthread_detach(pthread_t t);

int __real_pthread_create(pthread_t *t, const pthread_attr_t *attr, void *(*fn)(void *), void *arg) {
    return pthread_create(t, attr, fn, arg);
}

int __real_pthread_detach(pthread_t t) {
    return pthread_detach(t);
}
