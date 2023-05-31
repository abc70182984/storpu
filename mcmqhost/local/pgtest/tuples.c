#include "tuptable.h"

#include <stdlib.h>
#include <string.h>

static void tts_virtual_init(TupleTableSlot* slot) {}

static void tts_virtual_release(TupleTableSlot* slot) {}

static void tts_virtual_clear(TupleTableSlot* slot)
{
    if (TTS_SHOULDFREE(slot)) {
        VirtualTupleTableSlot* vslot = (VirtualTupleTableSlot*)slot;

        free(vslot->data);
        vslot->data = NULL;

        slot->tts_flags &= ~TTS_FLAG_SHOULDFREE;
    }

    slot->tts_nvalid = 0;
    slot->tts_flags |= TTS_FLAG_EMPTY;
    ItemPointerSetInvalid(&slot->tts_tid);
}

static void tts_virtual_materialize(TupleTableSlot* slot)
{
    VirtualTupleTableSlot* vslot = (VirtualTupleTableSlot*)slot;
    TupleDesc desc = slot->tts_tupleDescriptor;
    size_t sz = 0;
    char* data;

    /* already materialized */
    if (TTS_SHOULDFREE(slot)) return;

    /* compute size of memory required */
    for (int natt = 0; natt < desc->natts; natt++) {
        Form_pg_attribute att = TupleDescAttr(desc, natt);
        Datum val;

        if (att->attbyval || slot->tts_isnull[natt]) continue;

        val = slot->tts_values[natt];

        sz = att_align_nominal(sz, att->attalign);
        sz = att_addlength_datum(sz, att->attlen, val);
    }

    if (sz == 0) return;

    vslot->data = data = malloc(sz);
    slot->tts_flags |= TTS_FLAG_SHOULDFREE;

    for (int natt = 0; natt < desc->natts; natt++) {
        Form_pg_attribute att = TupleDescAttr(desc, natt);
        Datum val;

        if (att->attbyval || slot->tts_isnull[natt]) continue;

        val = slot->tts_values[natt];

        size_t data_length = 0;

        data = (char*)att_align_nominal(data, att->attalign);
        data_length = att_addlength_datum(data_length, att->attlen, val);

        memcpy(data, (void*)val, data_length);

        slot->tts_values[natt] = (Datum)data;
        data += data_length;
    }
}

const TupleTableSlotOps TTSOpsVirtual = {
    .base_slot_size = sizeof(VirtualTupleTableSlot),
    .init = tts_virtual_init,
    .release = tts_virtual_release,
    .clear = tts_virtual_clear,
    .getsomeattrs = NULL,
    .getsysattr = NULL,
    .materialize = tts_virtual_materialize,
    .copyslot = NULL,
    .copy_heap_tuple = NULL,
};

TupleTableSlot* MakeTupleTableSlot(TupleDesc tupleDesc,
                                   const TupleTableSlotOps* tts_ops)
{
    size_t basesz, allocsz;
    TupleTableSlot* slot;

    basesz = tts_ops->base_slot_size;

    if (tupleDesc)
        allocsz = MAXALIGN(basesz) +
                  MAXALIGN(tupleDesc->natts * sizeof(Datum)) +
                  MAXALIGN(tupleDesc->natts * sizeof(bool));
    else
        allocsz = basesz;

    slot = malloc(allocsz);
    memset(slot, 0, allocsz);
    *((const TupleTableSlotOps**)&slot->tts_ops) = tts_ops;
    slot->tts_flags |= TTS_FLAG_EMPTY;
    if (tupleDesc != NULL) slot->tts_flags |= TTS_FLAG_FIXED;
    slot->tts_tupleDescriptor = tupleDesc;
    slot->tts_nvalid = 0;

    if (tupleDesc != NULL) {
        slot->tts_values = (Datum*)(((char*)slot) + MAXALIGN(basesz));
        slot->tts_isnull = (bool*)(((char*)slot) + MAXALIGN(basesz) +
                                   MAXALIGN(tupleDesc->natts * sizeof(Datum)));
    }

    slot->tts_ops->init(slot);

    return slot;
}

void ExecDropSingleTupleTableSlot(TupleTableSlot* slot)
{
    ExecClearTuple(slot);
    slot->tts_ops->release(slot);
    if (!TTS_FIXED(slot)) {
        if (slot->tts_values) free(slot->tts_values);
        if (slot->tts_isnull) free(slot->tts_isnull);
    }
    free(slot);
}
