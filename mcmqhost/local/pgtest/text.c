#include "data_types.h"

#include <string.h>
#include <stdlib.h>

char* text_to_cstring(const text* t)
{
    int len = VARSIZE_ANY_EXHDR(t);
    char* result;

    result = (char*)malloc(len + 1);
    memcpy(result, VARDATA_ANY(t), len);
    result[len] = '\0';

    return result;
}
