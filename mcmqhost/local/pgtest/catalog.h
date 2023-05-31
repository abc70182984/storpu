#ifndef _CATALOG_H_
#define _CATALOG_H_

#include "data_types.h"
#include "schemas.h"

#define REL_OID_TABLE_TEST 1
#define REL_OID_INDEX_TEST 2

#define REL_OID_TABLE_TPCH_ORDERS   3
#define REL_OID_TABLE_TPCH_CUSTOMER 4
#define REL_OID_TABLE_TPCH_LINEITEM 5
#define REL_OID_TABLE_TPCH_NATION   6
#define REL_OID_TABLE_TPCH_PARTSUPP 7
#define REL_OID_TABLE_TPCH_PART     8
#define REL_OID_TABLE_TPCH_REGION   9
#define REL_OID_TABLE_TPCH_SUPPLIER 10

#define REL_OID_TABLE_TPCC_ORDERS 11
#define REL_OID_INDEX_TPCC_ORDERS 12

typedef struct {
    Oid aggfnoid;

    PGFunction aggtransfn;
    bool aggtransfnstrict;
    PGFunction aggfinalfn;
    PGFunction aggcombinefn;
    Form_pg_type aggtranstype;
    Form_pg_type aggrestype;
    const char* agginitval;
} FormData_pg_aggregate;

typedef FormData_pg_aggregate* Form_pg_aggregate;

__BEGIN_DECLS

Form_pg_aggregate catalog_get_aggregate(Oid id);

__END_DECLS

#endif
