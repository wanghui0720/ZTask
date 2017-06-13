#include "ztask.h"

#include "ztask_module.h"
#include "spinlock.h"

#include <assert.h>
#include <string.h>
#include <uv.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <malloc.h>
#define MAX_MODULE_TYPE 32

struct modules {
    int count;
    spinlock lock;
    const char * path;
    struct ztask_module m[MAX_MODULE_TYPE];
};

static struct modules * M = NULL;

static void *
_try_open(struct modules *m, const char * name) {
    const char *l;
    const char * path = m->path;
    size_t path_size = strlen(path);
    size_t name_size = strlen(name);

    size_t sz = path_size + name_size;
    //search path
    void * dl = NULL;
    char *tmp = alloca(sz);
    uv_lib_t lib;
    do
    {
        memset(tmp, 0, sz);
        while (*path == ';') path++;
        if (*path == '\0') break;
        l = strchr(path, ';');
        if (l == NULL) l = path + strlen(path);
        size_t len = l - path;
        size_t i;
        for (i = 0; path[i] != '?' && i < len; i++) {
            tmp[i] = path[i];
        }
        memcpy(tmp + i, name, name_size);
        if (path[i] == '?') {
            strncpy(tmp + i + name_size, path + i + 1, len - i - 1);
        }
        else {
            fprintf(stderr, "Invalid C service path\n");
            exit(1);
        }
        uv_dlopen(tmp, &lib);
        dl = lib.handle;
        //dl = dlopen(tmp, RTLD_NOW | RTLD_GLOBAL);
        path = l;
    } while (dl == NULL);

    if (dl == NULL) {
        fprintf(stderr, "try open %s failed : %s\n", name, lib.errmsg);
    }

    return dl;
}

static struct ztask_module *
_query(const char * name) {
    int i;
    for (i = 0; i < M->count; i++) {
        if (strcmp(M->m[i].name, name) == 0) {
            return &M->m[i];
        }
    }
    return NULL;
}

static void *
get_api(struct ztask_module *mod, const char *api_name) {
    size_t name_size = strlen(mod->name);
    size_t api_size = strlen(api_name);
    char *tmp = alloca(name_size + api_size + 1);
    memcpy(tmp, mod->name, name_size);
    memcpy(tmp + name_size, api_name, api_size + 1);
    char *ptr = strrchr(tmp, '.');
    if (ptr == NULL) {
        ptr = tmp;
    }
    else {
        ptr = ptr + 1;
    }
    uv_lib_t lib;
    lib.handle = mod->module;
    void *fp = NULL;
    uv_dlsym(&lib, ptr, &fp);
    return fp;
}

static int
open_sym(struct ztask_module *mod) {
    mod->create = get_api(mod, "_create");
    mod->init = get_api(mod, "_init");
    mod->release = get_api(mod, "_release");
    mod->signal = get_api(mod, "_signal");

    return mod->init == NULL;
}

struct ztask_module *
    ztask_module_query(const char * name) {
    struct ztask_module * result = _query(name);
    if (result)
        return result;

    SPIN_LOCK(M)

        result = _query(name); // double check

    if (result == NULL && M->count < MAX_MODULE_TYPE) {
        int index = M->count;
        void * dl = _try_open(M, name);
        if (dl) {
            M->m[index].name = name;
            M->m[index].module = dl;

            if (open_sym(&M->m[index]) == 0) {
                M->m[index].name = ztask_strdup(name);
                M->count++;
                result = &M->m[index];
            }
        }
    }

    SPIN_UNLOCK(M)

        return result;
}

void
ztask_module_insert(struct ztask_module *mod) {
    SPIN_LOCK(M)

        struct ztask_module * m = _query(mod->name);
    assert(m == NULL && M->count < MAX_MODULE_TYPE);
    int index = M->count;
    M->m[index] = *mod;
    ++M->count;

    SPIN_UNLOCK(M)
}

void *
ztask_module_instance_create(struct ztask_module *m) {
    if (m->create) {
        return m->create();
    }
    else {
        return (void *)(intptr_t)(~0);
    }
}

int
ztask_module_instance_init(struct ztask_module *m, void * inst, struct ztask_context *ctx, const char * parm, const size_t sz) {
    return m->init(inst, ctx, parm, sz);
}

void
ztask_module_instance_release(struct ztask_module *m, void *inst) {
    if (m->release) {
        m->release(inst);
    }
}

void
ztask_module_instance_signal(struct ztask_module *m, void *inst, int signal) {
    if (m->signal) {
        m->signal(inst, signal);
    }
}

void
ztask_module_init(const char *path) {
    struct modules *m = ztask_malloc(sizeof(*m));
    m->count = 0;
    m->path = ztask_strdup(path);

    SPIN_INIT(m);

    M = m;
}
