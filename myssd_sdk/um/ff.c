#include "ff.h"
#include <sys/stat.h>
#include <unistd.h>

FRESULT f_open(FIL* fp, const char* path, BYTE mode)
{
    const char* fmode = "";
    FILE* fil;

    if (mode & ~((FA_READ | FA_WRITE | FA_CREATE_ALWAYS | FA_OPEN_APPEND)))
        return FR_INVALID_PARAMETER;

    switch (mode & (FA_READ | FA_WRITE | FA_CREATE_ALWAYS | FA_OPEN_APPEND)) {
    case (FA_READ):
        fmode = "r";
        break;
    case (FA_READ | FA_WRITE):
        fmode = "r+";
        break;
    case (FA_CREATE_ALWAYS | FA_WRITE):
        fmode = "w";
        break;
    case (FA_CREATE_ALWAYS | FA_WRITE | FA_READ):
        fmode = "w+";
        break;
    case (FA_OPEN_APPEND | FA_WRITE):
        fmode = "a";
        break;
    case (FA_OPEN_APPEND | FA_WRITE | FA_READ):
        fmode = "a+";
        break;
    default:
        return FR_INVALID_PARAMETER;
    }

    fil = fopen(path, fmode);
    if (!fil) return FR_INT_ERR;

    *fp = fil;

    return FR_OK;
}

FRESULT f_close(FIL* fp)
{
    fclose(*fp);
    return FR_OK;
}

FRESULT f_read(FIL* fp, void* buff, UINT btr, UINT* br)
{
    size_t n;

    n = fread(buff, 1, btr, *fp);
    if (n < 0) return FR_INT_ERR;

    if (br) *br = (UINT)n;
    return FR_OK;
}

FRESULT f_write(FIL* fp, const void* buff, UINT btw, UINT* bw)
{
    size_t n;

    n = fwrite(buff, 1, btw, *fp);
    if (n < 0) return FR_INT_ERR;

    if (bw) *bw = (UINT)n;
    return FR_OK;
}

FRESULT f_lseek(FIL* fp, FSIZE_t ofs)
{
    (void)fseek(*fp, (long)ofs, SEEK_SET);
    return FR_OK;
}

FRESULT f_stat(const char* path, FILINFO* fno)
{
    struct stat sbuf;

    if (stat(path, &sbuf) < 0) return FR_INT_ERR;

    fno->fsize = (FSIZE_t)sbuf.st_size;

    return FR_OK;
}

FRESULT f_unlink(const char* path)
{
    if (unlink(path) < 0) return FR_INT_ERR;

    return FR_OK;
}
