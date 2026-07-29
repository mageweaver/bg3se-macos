#include "lua_gate.h"

#include <pthread.h>

static pthread_mutex_t s_gate;
static pthread_once_t s_gate_once = PTHREAD_ONCE_INIT;

static void gate_init(void) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&s_gate, &attr);
    pthread_mutexattr_destroy(&attr);
}

void lua_gate_lock(void) {
    pthread_once(&s_gate_once, gate_init);
    pthread_mutex_lock(&s_gate);
}

int lua_gate_trylock(void) {
    pthread_once(&s_gate_once, gate_init);
    return pthread_mutex_trylock(&s_gate) == 0;
}

void lua_gate_unlock(void) {
    pthread_mutex_unlock(&s_gate);
}
