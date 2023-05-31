#include <string.h>

#include "proto.h"
#include <utils.h>

#define ARG_MAX 10

typedef void (*console_command_handler_t)(int argc, const char** argv);

struct console_command {
    const char* name;
    console_command_handler_t handler;
};

void worker_report_stats(void);
static void report_stats(int argc, const char** argv);
static void sync_command(int argc, const char** argv);

static struct console_command command_list[] = {
    {"report", report_stats},
    {"dump_profile", profile_dump},
    {"sync", sync_command},
    {"mark_bad", bm_command_mark_bad},
    {"save_bad", bm_command_save_bad},
};

static struct console_command* get_command(const char* name)
{
    int i;

    for (i = 0; i < sizeof(command_list) / sizeof(0 [command_list]); i++) {
        struct console_command* cmd = &command_list[i];

        if (!strcmp(cmd->name, name)) return cmd;
    }

    return NULL;
}

static void report_stats(int argc, const char** argv)
{
    ftl_report_stats();
    worker_report_stats();
}

static void sync_command(int argc, const char** argv) { bm_shutdown(); }

static int parse_args(const u8* buf, size_t count, const char** argv,
                      int max_argc)
{
    static char cmdline[256];
    int argc;
    char* p;
    int beginning;

    if (count >= sizeof(cmdline)) return -1;

    memcpy(cmdline, buf, count);
    cmdline[count] = '\0';

    p = cmdline;
    argc = 0;
    beginning = TRUE;

    while (*p) {
        if (*p == ' ' || *p == '\n' || *p == '\r') {
            *p++ = '\0';
            beginning = TRUE;
            continue;
        }

        if (beginning) {
            if (argc >= max_argc) return -1;
            argv[argc++] = p;
            beginning = FALSE;
        }

        p++;
    }

    return argc;
}

static void handle_input(const u8* buf, size_t count)
{
    const char* argv[ARG_MAX];
    struct console_command* cmd;
    int argc;

    argc = parse_args(buf, count, argv, ARG_MAX);
    if (argc < 0) {
        printk("Command line or argument list too long\n");
        return;
    }

    if (argc == 0) {
        printk("Empty command line\n");
        return;
    }

    cmd = get_command(argv[0]);
    if (!cmd) {
        printk("Command '%s' not recognized\n", argv[0]);
        return;
    }

    cmd->handler(argc, argv);
}

void dbgcon_setup(void) { uart_set_recv_data_handler(handle_input); }
