#include <types.h>
#include <storpu/thread.h>
#include <storpu/completion.h>

void init_completion(struct completion* x)
{
    x->done = FALSE;
    mutex_init(&x->eventm, NULL);
    cond_init(&x->event, NULL);
}

void complete_all(struct completion* x)
{
    mutex_lock(&x->eventm);

    x->done = TRUE;
    cond_broadcast(&x->event);

    mutex_unlock(&x->eventm);
}

void wait_for_completion(struct completion* x)
{
    mutex_lock(&x->eventm);

    while (!x->done) {
        cond_wait(&x->event, &x->eventm);
    }

    mutex_unlock(&x->eventm);
}
