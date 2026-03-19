// env.c - liborion environment variables

#include "orion.h"

#define ENV_MAX     32          // max number of variables
#define ENV_BUFSZ   256         // max length of "KEY=VALUE" string

static char env_store[ENV_MAX][ENV_BUFSZ];
static int  env_count = 0;

// find index of 'name' in env_store (helper)
static int env_find(const char *name) {

    uint32_t nlen = (uint32_t)strlen(name);

    for (int i = 0; i < env_count; i++) {
        if (strncmp(env_store[i], name, nlen) == 0 && env_store[i][nlen] == '=') {
            return i;
        }
    }
    return -1;
}

// return enviroment variable
char *getenv(const char *name) {

    if (!name) return 0;

    int i = env_find(name);
    if (i < 0) return 0;

    return env_store[i] + strlen(name) + 1;    // skip "NAME="
}

// add/update enviroment variable
int setenv(const char *name, const char *value, int overwrite) {

    if (!name || !value) { errno = EINVAL; return -1; }

    int i = env_find(name);
    if (i >= 0) {

        if (!overwrite) return 0;                // already set: not overwriting

    } else {

        if (env_count >= ENV_MAX) { errno = ENOMEM; return -1; }
        i = env_count++;

    }
    snprintf(env_store[i], ENV_BUFSZ, "%s=%s", name, value);
    return 0;
}

// remove enviroment variable   
int unsetenv(const char *name) {

    if (!name) { errno = EINVAL; return -1; }

    int i = env_find(name);
    if (i < 0) return 0;                         // not found: POSIX says succeed anyway

    // shift remaining entries down to fill the gap
    for (int j = i; j < env_count - 1; j++) {
        memcpy(env_store[j], env_store[j + 1], ENV_BUFSZ);
    }
    env_count--;
    return 0;
}