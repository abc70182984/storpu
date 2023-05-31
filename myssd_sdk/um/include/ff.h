#ifndef _FF_H_
#define _FF_H_

#include <stdio.h>

/* These types MUST be 16-bit or 32-bit */
typedef int INT;
typedef unsigned int UINT;

/* This type MUST be 8-bit */
typedef unsigned char BYTE;

/* These types MUST be 16-bit */
typedef short SHORT;
typedef unsigned short WORD;
typedef unsigned short WCHAR;

/* These types MUST be 32-bit */
typedef long LONG;
typedef unsigned long DWORD;

/* This type MUST be 64-bit (Remove this for ANSI C (C89) compatibility) */
typedef unsigned long long QWORD;

typedef DWORD FSIZE_t;

typedef FILE* FIL;

typedef struct {
    FSIZE_t fsize;      /* File size */
    WORD fdate;         /* Modified date */
    WORD ftime;         /* Modified time */
    BYTE fattrib;       /* File attribute */
    char fname[12 + 1]; /* File name */
} FILINFO;

typedef enum {
    FR_OK = 0,   /* (0) Succeeded */
    FR_DISK_ERR, /* (1) A hard error occurred in the low level disk I/O layer */
    FR_INT_ERR,  /* (2) Assertion failed */
    FR_NOT_READY,    /* (3) The physical drive cannot work */
    FR_NO_FILE,      /* (4) Could not find the file */
    FR_NO_PATH,      /* (5) Could not find the path */
    FR_INVALID_NAME, /* (6) The path name format is invalid */
    FR_DENIED, /* (7) Access denied due to prohibited access or directory full
                */
    FR_EXIST,  /* (8) Access denied due to prohibited access */
    FR_INVALID_OBJECT,  /* (9) The file/directory object is invalid */
    FR_WRITE_PROTECTED, /* (10) The physical drive is write protected */
    FR_INVALID_DRIVE,   /* (11) The logical drive number is invalid */
    FR_NOT_ENABLED,     /* (12) The volume has no work area */
    FR_NO_FILESYSTEM,   /* (13) There is no valid FAT volume */
    FR_MKFS_ABORTED,    /* (14) The f_mkfs() aborted due to any problem */
    FR_TIMEOUT, /* (15) Could not get a grant to access the volume within
                   defined period */
    FR_LOCKED,  /* (16) The operation is rejected according to the file sharing
                   policy */
    FR_NOT_ENOUGH_CORE,     /* (17) LFN working buffer could not be allocated */
    FR_TOO_MANY_OPEN_FILES, /* (18) Number of open files > FF_FS_LOCK */
    FR_INVALID_PARAMETER    /* (19) Given parameter is invalid */
} FRESULT;

FRESULT f_open(FIL* fp, const char* path, BYTE mode);
FRESULT f_close(FIL* fp);
FRESULT f_read(FIL* fp, void* buff, UINT btr, UINT* br);
FRESULT f_write(FIL* fp, const void* buff, UINT btw, UINT* bw);
FRESULT f_lseek(FIL* fp, FSIZE_t ofs);
FRESULT f_stat(const char* path, FILINFO* fno);
FRESULT f_unlink(const char* path);

/* File access mode and open method flags (3rd argument of f_open) */
#define FA_READ          0x01
#define FA_WRITE         0x02
#define FA_OPEN_EXISTING 0x00
#define FA_CREATE_NEW    0x04
#define FA_CREATE_ALWAYS 0x08
#define FA_OPEN_ALWAYS   0x10
#define FA_OPEN_APPEND   0x30

#endif
