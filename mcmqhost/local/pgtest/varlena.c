#include "postgres.h"
#include "types.h"

#include <stdlib.h>
#include <string.h>

struct varlena* detoast_attr(struct varlena* attr)
{
    if (VARATT_IS_SHORT(attr)) {
        size_t data_size = VARSIZE_SHORT(attr) - VARHDRSZ_SHORT;
        size_t new_size = data_size + VARHDRSZ;
        struct varlena* new_attr;

        new_attr = (struct varlena*)malloc(new_size);
        SET_VARSIZE(new_attr, new_size);
        memcpy(VARDATA(new_attr), VARDATA_SHORT(attr), data_size);
        attr = new_attr;
    }

    return attr;
}

struct varlena* pg_detoast_datum(struct varlena* datum)
{
    if (VARATT_IS_EXTENDED(datum))
        return detoast_attr(datum);
    else
        return datum;
}

struct varlena* pg_detoast_datum_packed(struct varlena* datum)
{
    if (VARATT_IS_COMPRESSED(datum) || VARATT_IS_EXTERNAL(datum))
        return detoast_attr(datum);
    else
        return datum;
}
