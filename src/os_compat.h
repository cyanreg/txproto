#pragma once

#include "../config.h"

#ifdef HAVE_PTHREAD_SETNAME_NP
#include <pthread.h>
#include <string.h>
#include <errno.h>
void sp_set_thread_name_self(const char *name)
{
    pthread_t self = pthread_self();
    if (pthread_setname_np(self, name) == ERANGE) {
        char trunc_name[16];
        strncpy(trunc_name, name, 16);
        trunc_name[15] = '\0';
        pthread_setname_np(self, trunc_name);
    }
}
#else
void sp_set_thread_name_self(const char *name)
{
    return;
}
#endif
