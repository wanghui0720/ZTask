#include "coroutine.h"
#include "ztask.h"
#if defined(_WIN32) || defined(_WIN64)
#define _WIN32_WINNT 0x0501
#include <windows.h>
#else
#include "coroutine.h"
#include <ucontext.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <stdint.h>

#define STACK_SIZE (64<<10)

enum
{
    COROUTINE_NONE = 0,
    COROUTINE_SUSPEND,
    COROUTINE_RUNNING,
    COROUTINE_END,
};

struct coroutine
{
    int state;
    coenv_t env;
    coroutine_func func;
    void *result;
    void *context;
    void *ud;
    int type;
    int session;
    uint32_t source;
    const void * msg;
    size_t sz;
#if defined(_WIN32) || defined(_WIN64)
    HANDLE fiber;
#else
    char *stack;
    int stacksize;
    ucontext_t uctx;
#endif
    struct coroutine *main;
};

typedef struct coroutine * cort_t;

#if defined(_WIN32) || defined(_WIN64)
static void WINAPI _proxyfunc(void *p)
{
    cort_t co = (cort_t)p;
#else
static void _proxyfunc(uint32_t low32, uint32_t hi32)
{
    uintptr_t ptr = (uintptr_t)low32 | ((uintptr_t)hi32 << 32);
    cort_t co = (cort_t)ptr;
#endif
    co->result = co->func(co->env, co->context, co->ud, co->type, co->session, co->source, co->msg, co->sz);
    co->state = COROUTINE_END;
#if defined(_WIN32) || defined(_WIN64)
    SwitchToFiber(co->main->fiber);
#endif
}

static cort_t co_new(coenv_t env, cort_t main, coroutine_func func, void *context, void *ud, int type, int session, uint32_t source, const void * msg, size_t sz)
{
    struct coroutine *co = ztask_malloc(sizeof(*co));
    co->state = COROUTINE_SUSPEND;
    co->env = env;
    co->func = func;
    co->result = NULL;
    co->context = context;
    co->main = main;
    co->ud = ud;
    co->type = type;
    co->session = session;
    co->source = source;
    co->msg = msg;
    co->sz = sz;
#if defined(_WIN32) || defined(_WIN64)
    co->fiber = CreateFiber(0, _proxyfunc, co);
#else
    co->stacksize = STACK_SIZE;
    co->stack = ztask_malloc(co->stacksize);

    getcontext(&co->uctx);
    co->uctx.uc_stack.ss_sp = co->stack;
    co->uctx.uc_stack.ss_size = co->stacksize;
    co->uctx.uc_link = &main->uctx;

    uintptr_t ptr = (uintptr_t)co;
    makecontext(&co->uctx, (void(*)(void))_proxyfunc, 2, (uint32_t)ptr, (uint32_t)(ptr >> 32));
#endif

    return co;
}

static void co_delete(cort_t co)
{
#if defined(_WIN32) || defined(_WIN64)
    DeleteFiber(co->fiber);
#else
    free(co->stack);
#endif
    ztask_free(co);
}

static cort_t co_new_main()
{
    struct coroutine *co = ztask_malloc(sizeof(*co));
    co->state = COROUTINE_RUNNING;
    co->env = NULL;
    co->func = NULL;
    co->result = NULL;
    co->context = NULL;

#if defined(_WIN32) || defined(_WIN64)
    co->fiber = ConvertThreadToFiber(NULL);
#endif

    return co;
}

static void co_delete_main(cort_t co)
{
#if defined(_WIN32) || defined(_WIN64)
    ConvertFiberToThread();
#endif
    ztask_free(co);
}

static void co_switch(cort_t from, cort_t to)
{
#if defined(_WIN32) || defined(_WIN64)
    SwitchToFiber(to->fiber);
#else
    swapcontext(&from->uctx, &to->uctx);
#endif
}

/************************************************************************/
/* wrapper                                                              */
/************************************************************************/
struct coenviroment
{
    int nco;
    int cap;
    int running;
    cort_t *aco;
    cort_t main;
};

coenv_t coroutine_init()
{
    struct coenviroment *env = ztask_malloc(sizeof(*env));
    env->nco = 0;
    env->cap = 0;
    env->running = -1;
    env->aco = NULL;
    env->main = co_new_main();
    return env;
}

void coroutine_uninit(coenv_t env)
{
    int i;
    for (i = 0; i < env->cap; i++)
    {
        cort_t co = env->aco[i];
        if (co)
        {
            co_delete(co);
        }
    }

    ztask_free(env->aco);
    co_delete_main(env->main);
    ztask_free(env);
}

static int _insert_env(coenv_t env, cort_t co)
{
    int i;

    if (env->nco >= env->cap)
    {
        int newcap = (env->cap == 0) ? 16 : env->cap * 2;
        env->aco = ztask_realloc(env->aco, newcap * sizeof(cort_t));
        memset(env->aco + env->cap, 0, (newcap - env->cap) * sizeof(cort_t));
        env->cap = newcap;
    }

    for (i = 0; i < env->cap; i++)
    {
        int id = (i + env->nco) % env->cap;
        if (env->aco[id] == NULL)
        {
            env->aco[id] = co;
            env->nco++;
            return id;
        }
    }

    return -1;
}

int coroutine_new(coenv_t env, coroutine_func func, void *context, void *ud, int type, int session, uint32_t source, const void * msg, size_t sz)
{
    cort_t co = co_new(env, env->main, func, context, ud, type, session, source, msg, sz);
    return _insert_env(env, co);
}

void *coroutine_resume(coenv_t env, int id)
{
    if (0 <= id && id < env->cap)
    {
        cort_t co = env->aco[id];
        if (co && co->state == COROUTINE_SUSPEND)
        {
            env->running = id;
            co->state = COROUTINE_RUNNING;
            co_switch(env->main, co);
            void *ret = co->result;
            if (co->state == COROUTINE_END)
            {
                env->aco[id] = NULL;
                env->nco--;
                env->running = -1;
                co_delete(co);
            }
            return ret;
        }
    }
    return NULL;
}

void *coroutine_yield(coenv_t env)
{
    int id = env->running;
    if (0 <= id && id < env->cap)
    {
        cort_t co = env->aco[id];
        if (co && co->state == COROUTINE_RUNNING)
        {
            env->running = -1;
            co->state = COROUTINE_SUSPEND;
            co_switch(co, env->main);
            return co->result;
        }
    }
    return NULL;
}

int coroutine_current(coenv_t env) {
    return env->running;
}
