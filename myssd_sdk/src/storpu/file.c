#include <errno.h>
#include <string.h>

#include <types.h>
#include <utils.h>
#include <storpu.h>
#include <storpu/file.h>
#include <smp.h>
#include <storpu/thread.h>
#include <storpu/vm.h>
#include <page.h>

static ssize_t file_readwrite(int fd, void* buf, size_t count,
                              unsigned long offset, int do_write)
{
    struct storpu_ftl_task task;
    struct vumap_vir vir;
    struct vumap_phys phys;
    int r;

    vir.addr = (unsigned long)buf;
    vir.size = count;

    r = vm_vumap(current_thread->vm_context, &vir, 1, 0, !do_write, &phys, 1);
    if (r < 0) return r;

    if (!phys.addr) return -EFAULT;

    /* The buffer should be physically contiguous. */
    if (r != 1 || phys.size != count) return -EFAULT;

    memset(&task, 0, sizeof(task));
    if (fd == FD_HOST_MEM)
        task.type = do_write ? FTL_TYPE_HOST_WRITE : FTL_TYPE_HOST_READ;
    else
        task.type = do_write ? FTL_TYPE_FLASH_WRITE : FTL_TYPE_FLASH_READ;

    task.src_cpu = cpuid;

    if (fd >= 0) task.nsid = fd + 1;

    task.buf_phys = phys.addr;
    task.addr = offset;
    task.count = count;

    task.opaque = current_thread;

    set_current_state(THREAD_BLOCKED);
    enqueue_storpu_ftl_task(&task);
    schedule();

    if (task.retval) return -task.retval;

    return count;
}

ssize_t spu_read(int fd, void* buf, size_t count, unsigned long offset)
{
    if (fd == FD_SCRATCHPAD) {
        void* addr = map_scratchpad(offset, &count);

        if (!addr) return -EFAULT;
        memcpy(buf, addr, count);
        return count;
    }

    return file_readwrite(fd, buf, count, offset, FALSE);
}

ssize_t spu_write(int fd, const void* buf, size_t count, unsigned long offset)
{
    if (fd == FD_SCRATCHPAD) {
        void* addr = map_scratchpad(offset, &count);

        if (!addr) return -EFAULT;
        memcpy(addr, buf, count);
        return count;
    }

    return file_readwrite(fd, (void*)buf, count, offset, TRUE);
}

static int file_sync(int fd, int type)
{
    struct storpu_ftl_task task;

    if (fd < 0) return EINVAL;

    memset(&task, 0, sizeof(task));

    task.type = type;
    task.src_cpu = cpuid;

    task.nsid = fd + 1;

    task.opaque = current_thread;

    set_current_state(THREAD_BLOCKED);
    enqueue_storpu_ftl_task(&task);
    schedule();

    return task.retval;
}

int sys_fsync(int fd) { return file_sync(fd, FTL_TYPE_FLUSH); }

int sys_fdatasync(int fd) { return file_sync(fd, FTL_TYPE_FLUSH_DATA); }

void sys_sync(void) { file_sync(0, FTL_TYPE_SYNC); }
