#include "postgres.h"
#include "types.h"

#include <stdlib.h>
#include <string.h>

Datum datumCopy(Datum value, bool typByVal, int typLen)
{
    Datum res;

    if (typByVal)
        res = value;
    else if (typLen == -1) {
        /* It is a varlena datatype */
        struct varlena* vl = (struct varlena*)value;

        size_t realSize;
        char* resultptr;

        realSize = (size_t)VARSIZE_ANY(vl);
        resultptr = (char*)malloc(realSize);
        memcpy(resultptr, vl, realSize);
        res = (Datum)resultptr;
    } else {
        /* Pass by reference, but not varlena, so not toasted */
        size_t realSize;
        char* resultptr;

        realSize = typLen;

        resultptr = (char*)malloc(realSize);
        memcpy(resultptr, (void*)value, realSize);
        res = (Datum)resultptr;
    }
    return res;
}
