#ifndef _BUFFER_H_
#define _BUFFER_H_

#include "types.h"

typedef struct BufferData {
    BlockNumber blkno;
    char* bufpage;
} BufferData;

typedef BufferData* Buffer;

#define InvalidBuffer ((Buffer)NULL)

#define P_NEW InvalidBlockNumber

#define BUFFER_LOCK_UNLOCK    0
#define BUFFER_LOCK_SHARE     1
#define BUFFER_LOCK_EXCLUSIVE 2

#define BufferIsValid(buffer)   ((buffer) != InvalidBuffer)
#define BufferIsInvalid(buffer) ((buffer) == InvalidBuffer)

#define BufferGetPage(buffer)        ((buffer)->bufpage)
#define BufferGetBlockNumber(buffer) ((buffer)->blkno)

__BEGIN_DECLS

void LockBuffer(Buffer buf, int mode);
void ReleaseBuffer(Buffer buf);

__END_DECLS

#endif
