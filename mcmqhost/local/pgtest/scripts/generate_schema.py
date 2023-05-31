import psycopg2
import sys

TPCH_TABLES = [
    'customer',
    'lineitem',
    'nation',
    'orders',
    'partsupp',
    'part',
    'region',
    'supplier',
]

TPCC_TABLES = [
    'warehouse{}',
    'district{}',
    'customer{}',
    'history{}',
    'orders{}',
    'new_orders{}',
    'order_line{}',
    'stock{}',
    'item{}',
    'idx_customer{}',
    'idx_orders{}',
    'fkey_stock_2{}',
    'fkey_order_line_2{}',
    'fkey_history_1{}',
    'fkey_history_2{}',
]

TYPE_MAPPING = {
    20: 'int8',
    21: 'int2',
    23: 'int4',
    25: 'text',
    1042: 'char',  # char
    1043: 'text',  # varchar
    1082: 'date',  # date
    1114: 'timestamp',
    1700: 'decimal',  # decimal
}


def get_table_names(typ):
    if typ == 'tpch':
        return TPCH_TABLES
    elif typ == 'tpcc':
        return [x.format(1) for x in TPCC_TABLES]

    return []


def generate_schema(db, tables):
    conn = psycopg2.connect(f'dbname={db} user=postgres')

    cur = conn.cursor()

    for table in tables:
        cur.execute(
            f"SELECT oid,relfilenode,relpages FROM pg_class WHERE relname=%s;",
            [table])
        oid, filenode, relpages = cur.fetchone()

        print(f'/* Table "{table}", oid {oid}, relfilenode {filenode} */')
        print(f'TupleDescData {typ}_{table}_schema = {{')

        cur.execute(
            f"SELECT attname, attnum,atttypmod,attlen,attcacheoff,attbyval,attalign,atttypid FROM pg_attribute WHERE attrelid=%s;",
            [oid])
        atts = cur.fetchall()
        atts = [a for a in atts if a[1] >= 0]

        print(f'\t.natts = {len(atts)},')
        print(f'\t.relpages = {max(1, relpages)},')
        print('\t.attrs = {')

        for att in atts:
            print('\t\t{')
            print(f'\t\t\t.attname = "{att[0]}",')
            print(f'\t\t\t.attnum = {att[1]},')
            print(f'\t\t\t.atttypmod = {att[2]},')
            print(f'\t\t\t.attlen = {att[3]},')
            print(f'\t\t\t.attcacheoff = {att[4]},')
            print(f'\t\t\t.attbyval = {"true" if att[5] else "false"},')
            print(f"\t\t\t.attalign = '{att[6]}',")
            print(f"\t\t\t.atttypid = &type_{TYPE_MAPPING[att[7]]},")
            print('\t\t},')

        print('\t}')
        print('};')
        print('')

    conn.commit()

    cur.close()
    conn.close()


if __name__ == '__main__':
    typ = sys.argv[1]
    db = sys.argv[2]

    generate_schema(db, get_table_names(typ))
