#ifndef _SKEY_H_
#define _SKEY_H_

#include "fmgr.h"

typedef struct ScanKeyData {
    int sk_flags;
    AttrNumber sk_attno;
    uint16_t sk_strategy;
    FmgrInfo sk_func;
    Datum sk_argument;
} ScanKeyData;

typedef ScanKeyData* ScanKey;

static inline void ScanKeyInit(ScanKey entry, AttrNumber attributeNumber,
                               uint16_t strategy, PGFunction func,
                               Datum argument)
{
    entry->sk_flags = 0;
    entry->sk_attno = attributeNumber;
    entry->sk_strategy = strategy;
    entry->sk_func.fn_addr = func;
    entry->sk_argument = argument;
}

#endif
