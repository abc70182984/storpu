#ifndef _TUPTABLE_H_
#define _TUPTABLE_H_

#include "types.h"
#include "tupdesc.h"
#include "heap.h"

/* true = slot is empty */
#define TTS_FLAG_EMPTY  (1 << 1)
#define TTS_EMPTY(slot) (((slot)->tts_flags & TTS_FLAG_EMPTY) != 0)

/* should pfree tuple "owned" by the slot? */
#define TTS_FLAG_SHOULDFREE  (1 << 2)
#define TTS_SHOULDFREE(slot) (((slot)->tts_flags & TTS_FLAG_SHOULDFREE) != 0)

/* saved state for slot_deform_heap_tuple */
#define TTS_FLAG_SLOW  (1 << 3)
#define TTS_SLOW(slot) (((slot)->tts_flags & TTS_FLAG_SLOW) != 0)

/* fixed tuple descriptor */
#define TTS_FLAG_FIXED  (1 << 4)
#define TTS_FIXED(slot) (((slot)->tts_flags & TTS_FLAG_FIXED) != 0)

struct TupleTableSlotOps;
typedef struct TupleTableSlotOps TupleTableSlotOps;

/* base tuple table slot type */
typedef struct TupleTableSlot {
    uint16_t tts_flags;    /* Boolean states */
    AttrNumber tts_nvalid; /* # of valid values in tts_values */
    const TupleTableSlotOps* const tts_ops; /* implementation of slot */
    TupleDesc tts_tupleDescriptor;          /* slot's tuple descriptor */
    Datum* tts_values;                      /* current per-attribute values */
    bool* tts_isnull;        /* current per-attribute isnull flags */
    ItemPointerData tts_tid; /* stored tuple's tid */
    Oid tts_tableOid;        /* table oid of tuple */
} TupleTableSlot;

/* routines for a TupleTableSlot implementation */
struct TupleTableSlotOps {
    /* Minimum size of the slot */
    size_t base_slot_size;

    /* Initialization. */
    void (*init)(TupleTableSlot* slot);

    /* Destruction. */
    void (*release)(TupleTableSlot* slot);

    /*
     * Clear the contents of the slot. Only the contents are expected to be
     * cleared and not the tuple descriptor. Typically an implementation of
     * this callback should free the memory allocated for the tuple contained
     * in the slot.
     */
    void (*clear)(TupleTableSlot* slot);

    /*
     * Fill up first natts entries of tts_values and tts_isnull arrays with
     * values from the tuple contained in the slot. The function may be called
     * with natts more than the number of attributes available in the tuple,
     * in which case it should set tts_nvalid to the number of returned
     * columns.
     */
    void (*getsomeattrs)(TupleTableSlot* slot, int natts);

    /*
     * Returns value of the given system attribute as a datum and sets isnull
     * to false, if it's not NULL. Throws an error if the slot type does not
     * support system attributes.
     */
    Datum (*getsysattr)(TupleTableSlot* slot, int attnum, bool* isnull);

    /*
     * Make the contents of the slot solely depend on the slot, and not on
     * underlying resources (like another memory context, buffers, etc).
     */
    void (*materialize)(TupleTableSlot* slot);

    /*
     * Copy the contents of the source slot into the destination slot's own
     * context. Invoked using callback of the destination slot.
     */
    void (*copyslot)(TupleTableSlot* dstslot, TupleTableSlot* srcslot);

    /*
     * Return a heap tuple "owned" by the slot. It is slot's responsibility to
     * free the memory consumed by the heap tuple. If the slot can not "own" a
     * heap tuple, it should not implement this callback and should set it as
     * NULL.
     */
    HeapTuple (*get_heap_tuple)(TupleTableSlot* slot);

    /*
     * Return a copy of heap tuple representing the contents of the slot. The
     * copy needs to be palloc'd in the current memory context. The slot
     * itself is expected to remain unaffected. It is *not* expected to have
     * meaningful "system columns" in the copy. The copy is not be "owned" by
     * the slot i.e. the caller has to take responsibility to free memory
     * consumed by the slot.
     */
    HeapTuple (*copy_heap_tuple)(TupleTableSlot* slot);
};

extern const TupleTableSlotOps TTSOpsVirtual;

#define TTS_IS_VIRTUAL(slot) ((slot)->tts_ops == &TTSOpsVirtual)

typedef struct VirtualTupleTableSlot {
    TupleTableSlot base;

    char* data; /* data for materialized slots */
} VirtualTupleTableSlot;

__BEGIN_DECLS

static inline TupleTableSlot* ExecClearTuple(TupleTableSlot* slot)
{
    slot->tts_ops->clear(slot);

    return slot;
}

static inline void ExecMaterializeSlot(TupleTableSlot* slot)
{
    slot->tts_ops->materialize(slot);
}

TupleTableSlot* MakeTupleTableSlot(TupleDesc tupleDesc,
                                   const TupleTableSlotOps* tts_ops);
void ExecDropSingleTupleTableSlot(TupleTableSlot* slot);

__END_DECLS

#endif
