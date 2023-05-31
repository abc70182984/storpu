import psycopg2
import sys
import os
import shutil

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


def get_table_names(typ):
    if typ == 'tpch':
        return TPCH_TABLES
    elif typ == 'tpcc':
        return [x.format(1) for x in TPCC_TABLES]

    return []


def collect_tables(db, tables):
    conn = psycopg2.connect(f'dbname={db} user=postgres')

    cur = conn.cursor()

    data_dir = f'../data/{db}'
    if not os.path.exists(data_dir):
        os.makedirs(data_dir)

    for table in tables:
        cur.execute(f"SELECT oid FROM pg_database WHERE datname=%s;", [db])
        db_oid = cur.fetchone()[0]

        cur.execute(f"SELECT oid,relfilenode FROM pg_class WHERE relname=%s;",
                    [table])
        oid, filenode = cur.fetchone()

        dest = f'{data_dir}/{table}'
        src = f'/home/postgres/postgres/data/base/{db_oid}/{filenode}'
        print(f'{src} -> {dest}')
        shutil.copyfile(src, dest)

    conn.commit()

    cur.close()
    conn.close()


if __name__ == '__main__':
    typ = sys.argv[1]
    db = sys.argv[2]

    collect_tables(db, get_table_names(typ))
