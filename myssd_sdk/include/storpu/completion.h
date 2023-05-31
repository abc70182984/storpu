#ifndef _STORPU_COMPLETION_H_
#define _STORPU_COMPLETION_H_

struct completion {
    int done;
    mutex_t eventm;
    cond_t event;
};

void init_completion(struct completion* completion);
void complete_all(struct completion* completion);
void wait_for_completion(struct completion* completion);

#endif
