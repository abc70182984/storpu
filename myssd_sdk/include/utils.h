#ifndef _UTILS_H_
#define _UTILS_H_

#include "xil_printf.h"
#include <const.h>

#define __WARN(file, line, caller, format...)                          \
    do {                                                               \
        xil_printf("WARNING at %s:%d %p\n", (file), (line), (caller)); \
        xil_printf(format);                                            \
    } while (0)

#define WARN(condition, format...)                                           \
    ({                                                                       \
        int __ret_warn_on = !!(condition);                                   \
        if (unlikely(__ret_warn_on))                                         \
            __WARN(__FILE__, __LINE__, __builtin_return_address(0), format); \
        unlikely(__ret_warn_on);                                             \
    })

#define WARN_ONCE(condition, format...)                   \
    ({                                                    \
        static int __already_done;                        \
        int __ret_do_once = !!(condition);                \
                                                          \
        if (unlikely(__ret_do_once && !__already_done)) { \
            __already_done = 1;                           \
            WARN(1, format);                              \
        }                                                 \
        unlikely(__ret_do_once);                          \
    })

int printk(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

void panic(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
