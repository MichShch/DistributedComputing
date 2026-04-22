#!/usr/bin/env python3
import argparse
import os
import sys
from collections import defaultdict

try:
    import psycopg2
    from psycopg2 import sql
except ImportError:  # pragma: no cover - runtime dependency
    print('ERROR: missing dependency psycopg2. Install with: pip install psycopg2-binary', file=sys.stderr)
    sys.exit(1)


def parse_env_file(path):
    data = {}
    if not path:
        return data
    if not os.path.exists(path):
        raise FileNotFoundError(f'Config file not found: {path}')
    with open(path, 'r', encoding='utf-8') as f:
        for raw in f:
            line = raw.strip()
            if not line or line.startswith('#'):
                continue
            if line.startswith('export '):
                line = line[len('export ') :]
            if '=' not in line:
                continue
            key, val = line.split('=', 1)
            key = key.strip()
            val = val.strip().strip(''').strip(''')
            data[key] = val
    return data


def pick_value(cli_value, env, config, key, default=None):
    if cli_value is not None:
        return cli_value
    if key in env:
        return env[key]
    if key in config:
        return config[key]
    return default


def connect_db(params, dbname):
    return psycopg2.connect(
        host=params['host'],
        port=params['port'],
        user=params['user'],
        password=params['password'],
        dbname=dbname,
        sslmode=params.get('sslmode'),
    )


def database_exists(conn, dbname):
    with conn.cursor() as cur:
        cur.execute('SELECT 1 FROM pg_database WHERE datname = %s', (dbname,))
        return cur.fetchone() is not None


def create_database(conn, dbname):
    if conn.get_transaction_status() != psycopg2.extensions.TRANSACTION_STATUS_IDLE:
        conn.rollback()
    conn.autocommit = True
    with conn.cursor() as cur:
        cur.execute(sql.SQL('CREATE DATABASE {}').format(sql.Identifier(dbname)))


def create_schema(conn):
    with conn.cursor() as cur:
        cur.execute('SELECT 1 FROM pg_type WHERE typname = 'agent_status'')
        if cur.fetchone() is None:
            cur.execute(
                'CREATE TYPE agent_status AS ENUM ('Idle', 'Busy', 'Offline')'
            )
        cur.execute('SELECT 1 FROM pg_type WHERE typname = 'task_state'')
        if cur.fetchone() is None:
            cur.execute(
                'CREATE TYPE task_state AS ENUM ('Queued', 'Running', 'Succeeded', 'Failed', 'Canceled')'
            )
        cur.execute(
            '''
            CREATE TABLE IF NOT EXISTS agents (
                agent_id TEXT PRIMARY KEY,
                os TEXT NOT NULL,
                version TEXT NOT NULL,
                resources_cpu_cores INT NOT NULL,
                resources_ram_mb INT NOT NULL,
                resources_slots INT NOT NULL,
                status agent_status NOT NULL,
                last_heartbeat TIMESTAMPTZ NOT NULL
            )
            '''
        )
        cur.execute(
            '''
            CREATE TABLE IF NOT EXISTS tasks (
                task_id BIGSERIAL PRIMARY KEY,
                state task_state NOT NULL,
                command TEXT NOT NULL,
                args JSONB NOT NULL,
                env JSONB NOT NULL,
                constraints JSONB NOT NULL DEFAULT '{}'::jsonb,
                timeout_sec INT,
                assigned_agent TEXT,
                created_at TIMESTAMPTZ NOT NULL,
                started_at TIMESTAMPTZ,
                finished_at TIMESTAMPTZ,
                exit_code INT,
                error_message TEXT,
                CONSTRAINT tasks_assigned_agent_fk FOREIGN KEY (assigned_agent)
                    REFERENCES agents(agent_id)
            )
            '''
        )
        cur.execute(
            '''
            CREATE TABLE IF NOT EXISTS task_assignments (
                id BIGSERIAL PRIMARY KEY,
                task_id BIGINT NOT NULL,
                agent_id TEXT NOT NULL,
                assigned_at TIMESTAMPTZ NOT NULL,
                unassigned_at TIMESTAMPTZ,
                reason TEXT,
                CONSTRAINT task_assignments_task_id_fk FOREIGN KEY (task_id)
                    REFERENCES tasks(task_id) ON DELETE CASCADE,
                CONSTRAINT task_assignments_agent_id_fk FOREIGN KEY (agent_id)
                    REFERENCES agents(agent_id) ON DELETE CASCADE
            )
            '''
        )
        cur.execute('CREATE INDEX IF NOT EXISTS idx_agents_status ON agents(status)')
        cur.execute(
            'CREATE INDEX IF NOT EXISTS idx_agents_last_heartbeat ON agents(last_heartbeat)'
        )
        cur.execute(
            'CREATE INDEX IF NOT EXISTS idx_tasks_state_created_at ON tasks(state, created_at)'
        )
        cur.execute(
            'CREATE INDEX IF NOT EXISTS idx_tasks_assigned_agent ON tasks(assigned_agent)'
        )
        cur.execute('CREATE INDEX IF NOT EXISTS idx_tasks_created_at ON tasks(created_at)')
        cur.execute(
            'CREATE INDEX IF NOT EXISTS idx_tasks_constraints ON tasks USING GIN (constraints)'
        )
        cur.execute(
            'CREATE INDEX IF NOT EXISTS idx_task_assignments_task_id_assigned_at '
            'ON task_assignments(task_id, assigned_at)'
        )
        cur.execute(
            'CREATE INDEX IF NOT EXISTS idx_task_assignments_agent_id_assigned_at '
            'ON task_assignments(agent_id, assigned_at)'
        )
    conn.commit()


def expected_schema():
    return {
        'enums': {
            'agent_status': ['Idle', 'Busy', 'Offline'],
            'task_state': ['Queued', 'Running', 'Succeeded', 'Failed', 'Canceled'],
        },
        'tables': {
            'agents': {
                'columns': {
                    'agent_id': ('text', False),
                    'os': ('text', False),
                    'version': ('text', False),
                    'resources_cpu_cores': ('integer', False),
                    'resources_ram_mb': ('integer', False),
                    'resources_slots': ('integer', False),
                    'status': ('agent_status', False),
                    'last_heartbeat': ('timestamp with time zone', False),
                },
                'primary_key': ['agent_id'],
                'foreign_keys': [],
                'indexes': {
                    'idx_agents_status': ['status'],
                    'idx_agents_last_heartbeat': ['last_heartbeat'],
                },
            },
            'tasks': {
                'columns': {
                    'task_id': ('bigserial', False),
                    'state': ('task_state', False),
                    'command': ('text', False),
                    'args': ('jsonb', False),
                    'env': ('jsonb', False),
                    'constraints': ('jsonb', False),
                    'timeout_sec': ('integer', True),
                    'assigned_agent': ('text', True),
                    'created_at': ('timestamp with time zone', False),
                    'started_at': ('timestamp with time zone', True),
                    'finished_at': ('timestamp with time zone', True),
                    'exit_code': ('integer', True),
                    'error_message': ('text', True),
                },
                'primary_key': ['task_id'],
                'foreign_keys': [
                    ('assigned_agent', 'agents', 'agent_id', 'NO ACTION'),
                ],
                'indexes': {
                    'idx_tasks_state_created_at': ['state', 'created_at'],
                    'idx_tasks_assigned_agent': ['assigned_agent'],
                    'idx_tasks_created_at': ['created_at'],
                    'idx_tasks_constraints': ['constraints'],
                },
            },
            'task_assignments': {
                'columns': {
                    'id': ('bigserial', False),
                    'task_id': ('bigint', False),
                    'agent_id': ('text', False),
                    'assigned_at': ('timestamp with time zone', False),
                    'unassigned_at': ('timestamp with time zone', True),
                    'reason': ('text', True),
                },
                'primary_key': ['id'],
                'foreign_keys': [
                    ('task_id', 'tasks', 'task_id', 'CASCADE'),
                    ('agent_id', 'agents', 'agent_id', 'CASCADE'),
                ],
                'indexes': {
                    'idx_task_assignments_task_id_assigned_at': [
                        'task_id',
                        'assigned_at',
                    ],
                    'idx_task_assignments_agent_id_assigned_at': [
                        'agent_id',
                        'assigned_at',
                    ],
                },
            },
        },
    }


def table_exists(conn, table):
    with conn.cursor() as cur:
        cur.execute(
            '''
            SELECT 1
            FROM information_schema.tables
            WHERE table_schema = 'public' AND table_name = %s
            ''',
            (table,),
        )
        return cur.fetchone() is not None


def column_exists(conn, table, column):
    with conn.cursor() as cur:
        cur.execute(
            '''
            SELECT 1
            FROM information_schema.columns
            WHERE table_schema = 'public'
              AND table_name = %s
              AND column_name = %s
            ''',
            (table, column),
        )
        return cur.fetchone() is not None


def constraint_exists(conn, table, constraint_name):
    with conn.cursor() as cur:
        cur.execute(
            '''
            SELECT 1
            FROM information_schema.table_constraints
            WHERE table_schema = 'public'
              AND table_name = %s
              AND constraint_name = %s
            ''',
            (table, constraint_name),
        )
        return cur.fetchone() is not None


def index_exists(conn, index_name):
    with conn.cursor() as cur:
        cur.execute(
            '''
            SELECT 1
            FROM pg_class c
            JOIN pg_namespace n ON n.oid = c.relnamespace
            WHERE n.nspname = 'public'
              AND c.relkind = 'i'
              AND c.relname = %s
            ''',
            (index_name,),
        )
        return cur.fetchone() is not None


def ensure_column(conn, table, column, ddl):
    if column_exists(conn, table, column):
        return False
    with conn.cursor() as cur:
        cur.execute(f'ALTER TABLE {table} ADD COLUMN {column} {ddl}')
    return True


def ensure_fk(conn, table, constraint_name, ddl):
    if constraint_exists(conn, table, constraint_name):
        return False
    with conn.cursor() as cur:
        cur.execute(f'ALTER TABLE {table} ADD CONSTRAINT {constraint_name} {ddl}')
    return True


def ensure_index(conn, ddl, index_name):
    if index_exists(conn, index_name):
        return False
    with conn.cursor() as cur:
        cur.execute(ddl)
    return True


def migrate_schema(conn):
    changed = False
    if table_exists(conn, 'tasks') and not column_exists(conn, 'tasks', 'constraints'):
        with conn.cursor() as cur:
            cur.execute(
                'ALTER TABLE tasks ADD COLUMN constraints JSONB NOT NULL DEFAULT '{}'::jsonb'
            )
        changed = True
        if table_exists(conn, 'task_constraints'):
            with conn.cursor() as cur:
                cur.execute(
                    '''
                    UPDATE tasks t
                    SET constraints = jsonb_strip_nulls(
                        jsonb_build_object(
                            'os', c.os,
                            'cpu_cores', c.cpu_cores,
                            'ram_mb', c.ram_mb,
                            'labels', c.labels
                        )
                    )
                    FROM task_constraints c
                    WHERE t.task_id = c.task_id
                    '''
                )
        changed = True

    if table_exists(conn, 'agents'):
        changed = ensure_column(conn, 'agents', 'resources_slots', 'INT NOT NULL DEFAULT 1') or changed
        changed = ensure_index(
            conn,
            'CREATE INDEX IF NOT EXISTS idx_agents_status ON agents(status)',
            'idx_agents_status',
        ) or changed
        changed = ensure_index(
            conn,
            'CREATE INDEX IF NOT EXISTS idx_agents_last_heartbeat ON agents(last_heartbeat)',
            'idx_agents_last_heartbeat',
        ) or changed

    if table_exists(conn, 'tasks') and column_exists(conn, 'tasks', 'constraints'):
        changed = ensure_column(conn, 'tasks', 'assigned_agent', 'TEXT') or changed
        changed = ensure_fk(
            conn,
            'tasks',
            'tasks_assigned_agent_fk',
            'FOREIGN KEY (assigned_agent) REFERENCES agents(agent_id)',
        ) or changed
        changed = ensure_index(
            conn,
            'CREATE INDEX IF NOT EXISTS idx_tasks_state_created_at ON tasks(state, created_at)',
            'idx_tasks_state_created_at',
        ) or changed
        changed = ensure_index(
            conn,
            'CREATE INDEX IF NOT EXISTS idx_tasks_assigned_agent ON tasks(assigned_agent)',
            'idx_tasks_assigned_agent',
        ) or changed
        changed = ensure_index(
            conn,
            'CREATE INDEX IF NOT EXISTS idx_tasks_created_at ON tasks(created_at)',
            'idx_tasks_created_at',
        ) or changed
        changed = ensure_index(
            conn,
            'CREATE INDEX IF NOT EXISTS idx_tasks_constraints ON tasks USING GIN (constraints)',
            'idx_tasks_constraints',
        ) or changed

    if not table_exists(conn, 'task_assignments'):
        with conn.cursor() as cur:
            cur.execute(
                '''
                CREATE TABLE task_assignments (
                    id BIGSERIAL PRIMARY KEY,
                    task_id BIGINT NOT NULL,
                    agent_id TEXT NOT NULL,
                    assigned_at TIMESTAMPTZ NOT NULL,
                    unassigned_at TIMESTAMPTZ,
                    reason TEXT,
                    CONSTRAINT task_assignments_task_id_fk FOREIGN KEY (task_id)
                        REFERENCES tasks(task_id) ON DELETE CASCADE,
                    CONSTRAINT task_assignments_agent_id_fk FOREIGN KEY (agent_id)
                        REFERENCES agents(agent_id) ON DELETE CASCADE
                )
                '''
            )
        changed = True
    else:
        changed = ensure_column(conn, 'task_assignments', 'unassigned_at', 'TIMESTAMPTZ') or changed
        changed = ensure_column(conn, 'task_assignments', 'reason', 'TEXT') or changed
        changed = ensure_fk(
            conn,
            'task_assignments',
            'task_assignments_task_id_fk',
            'FOREIGN KEY (task_id) REFERENCES tasks(task_id) ON DELETE CASCADE',
        ) or changed
        changed = ensure_fk(
            conn,
            'task_assignments',
            'task_assignments_agent_id_fk',
            'FOREIGN KEY (agent_id) REFERENCES agents(agent_id) ON DELETE CASCADE',
        ) or changed

    if table_exists(conn, 'task_assignments'):
        changed = ensure_index(
            conn,
            'CREATE INDEX IF NOT EXISTS idx_task_assignments_task_id_assigned_at '
            'ON task_assignments(task_id, assigned_at)',
            'idx_task_assignments_task_id_assigned_at',
        ) or changed
        changed = ensure_index(
            conn,
            'CREATE INDEX IF NOT EXISTS idx_task_assignments_agent_id_assigned_at '
            'ON task_assignments(agent_id, assigned_at)',
            'idx_task_assignments_agent_id_assigned_at',
        ) or changed

    if changed:
        conn.commit()


def fetch_enums(conn):
    enums = defaultdict(list)
    with conn.cursor() as cur:
        cur.execute(
            '''
            SELECT t.typname, e.enumlabel
            FROM pg_type t
            JOIN pg_enum e ON t.oid = e.enumtypid
            JOIN pg_namespace n ON n.oid = t.typnamespace
            WHERE n.nspname = 'public'
            ORDER BY t.typname, e.enumsortorder
            '''
        )
        for typname, enumlabel in cur.fetchall():
            enums[typname].append(enumlabel)
    return dict(enums)


def fetch_columns(conn, table):
    with conn.cursor() as cur:
        cur.execute(
            '''
            SELECT column_name, data_type, is_nullable, udt_name, column_default
            FROM information_schema.columns
            WHERE table_schema = 'public' AND table_name = %s
            ''',
            (table,),
        )
        return cur.fetchall()


def fetch_pk(conn, table):
    with conn.cursor() as cur:
        cur.execute(
            '''
            SELECT kcu.column_name
            FROM information_schema.table_constraints tc
            JOIN information_schema.key_column_usage kcu
                ON tc.constraint_name = kcu.constraint_name
                AND tc.table_schema = kcu.table_schema
            WHERE tc.table_schema = 'public'
              AND tc.table_name = %s
              AND tc.constraint_type = 'PRIMARY KEY'
            ORDER BY kcu.ordinal_position
            ''',
            (table,),
        )
        return [r[0] for r in cur.fetchall()]


def fetch_fks(conn, table):
    with conn.cursor() as cur:
        cur.execute(
            '''
            SELECT kcu.column_name, ccu.table_name, ccu.column_name, rc.delete_rule
            FROM information_schema.table_constraints tc
            JOIN information_schema.key_column_usage kcu
              ON tc.constraint_name = kcu.constraint_name
             AND tc.table_schema = kcu.table_schema
            JOIN information_schema.referential_constraints rc
              ON rc.constraint_name = tc.constraint_name
             AND rc.constraint_schema = tc.table_schema
            JOIN information_schema.constraint_column_usage ccu
              ON ccu.constraint_name = tc.constraint_name
             AND ccu.table_schema = tc.table_schema
            WHERE tc.table_schema = 'public'
              AND tc.table_name = %s
              AND tc.constraint_type = 'FOREIGN KEY'
            ''',
            (table,),
        )
        return [(r[0], r[1], r[2], r[3]) for r in cur.fetchall()]


def fetch_indexes(conn, table):
    with conn.cursor() as cur:
        cur.execute(
            '''
            SELECT i.relname AS index_name,
                   array_agg(a.attname ORDER BY x.ordinality) AS columns
            FROM pg_class t
            JOIN pg_index idx ON t.oid = idx.indrelid
            JOIN pg_class i ON i.oid = idx.indexrelid
            JOIN unnest(idx.indkey) WITH ORDINALITY AS x(attnum, ordinality) ON true
            JOIN pg_attribute a ON a.attrelid = t.oid AND a.attnum = x.attnum
            WHERE t.relnamespace = 'public'::regnamespace
              AND t.relname = %s
              AND NOT idx.indisprimary
            GROUP BY i.relname
            ''',
            (table,),
        )
        return {r[0]: r[1] for r in cur.fetchall()}


def compare_schema(conn):
    diffs = []
    expected = expected_schema()

    actual_enums = fetch_enums(conn)
    for enum_name, enum_vals in expected['enums'].items():
        if enum_name not in actual_enums:
            diffs.append(f'ENUM '{enum_name}' отсутствует')
            continue
        if actual_enums[enum_name] != enum_vals:
            diffs.append(
                f'ENUM '{enum_name}' значения отличаются: '
                f'ожидается {enum_vals}, фактически {actual_enums[enum_name]}'
            )

    with conn.cursor() as cur:
        cur.execute(
            '''
            SELECT table_name
            FROM information_schema.tables
            WHERE table_schema = 'public' AND table_type = 'BASE TABLE'
            '''
        )
        actual_tables = {r[0] for r in cur.fetchall()}

    for table, spec in expected['tables'].items():
        if table not in actual_tables:
            diffs.append(f'Таблица '{table}' отсутствует')
            continue

        cols = fetch_columns(conn, table)
        actual_cols = {}
        for name, data_type, is_nullable, udt_name, column_default in cols:
            if data_type == 'USER-DEFINED':
                normalized = udt_name
            else:
                normalized = data_type
            actual_cols[name] = {
                'type': normalized,
                'nullable': is_nullable == 'YES',
                'default': column_default or '',
            }

        for col_name, (col_type, col_nullable) in spec['columns'].items():
            if col_name not in actual_cols:
                diffs.append(f'Таблица '{table}': колонка '{col_name}' отсутствует')
                continue
            actual = actual_cols[col_name]
            if col_type == 'bigserial':
                if actual['type'] != 'bigint' or 'nextval(' not in actual['default']:
                    diffs.append(
                        f'Таблица '{table}': колонка '{col_name}' тип отличается '
                        f'(ожидается bigserial)'
                    )
            else:
                if actual['type'] != col_type:
                    diffs.append(
                        f'Таблица '{table}': колонка '{col_name}' тип отличается '
                        f'(ожидается {col_type}, фактически {actual['type']})'
                    )
            if actual['nullable'] != col_nullable:
                diffs.append(
                    f'Таблица '{table}': колонка '{col_name}' nullability отличается '
                    f'(ожидается {'NULL' if col_nullable else 'NOT NULL'})'
                )

        expected_cols = set(spec['columns'].keys())
        extra_cols = set(actual_cols.keys()) - expected_cols
        for col in sorted(extra_cols):
            diffs.append(f'Таблица '{table}': лишняя колонка '{col}'')

        pk = fetch_pk(conn, table)
        if pk != spec['primary_key']:
            diffs.append(
                f'Таблица '{table}': PK отличается (ожидается {spec['primary_key']}, фактически {pk})'
            )

        actual_fks = fetch_fks(conn, table)
        expected_fks = spec['foreign_keys']
        missing_fks = [fk for fk in expected_fks if fk not in actual_fks]
        for fk in missing_fks:
            diffs.append(
                f'Таблица '{table}': FK отсутствует ({fk[0]} -> {fk[1]}.{fk[2]}, ON DELETE {fk[3]})'
            )

        actual_indexes = fetch_indexes(conn, table)
        for idx_name, idx_cols in spec['indexes'].items():
            if idx_name not in actual_indexes:
                diffs.append(f'Таблица '{table}': индекс '{idx_name}' отсутствует')
                continue
            if actual_indexes[idx_name] != idx_cols:
                diffs.append(
                    f'Таблица '{table}': индекс '{idx_name}' отличается '
                    f'(ожидается {idx_cols}, фактически {actual_indexes[idx_name]})'
                )

    return diffs


def main():
    parser = argparse.ArgumentParser(description='Init/check PostgreSQL database schema.')
    parser.add_argument('--config', help='Path to config file with DB_* variables')
    parser.add_argument('--host')
    parser.add_argument('--port', type=int)
    parser.add_argument('--user')
    parser.add_argument('--password')
    parser.add_argument('--dbname')
    parser.add_argument('--sslmode')
    args = parser.parse_args()

    env = os.environ
    config_path = args.config or env.get('DB_CONFIG')
    config = parse_env_file(config_path) if config_path else {}

    params = {
        'host': pick_value(args.host, env, config, 'DB_HOST', 'localhost'),
        'port': int(pick_value(args.port, env, config, 'DB_PORT', 5432)),
        'user': pick_value(args.user, env, config, 'DB_USER'),
        'password': pick_value(args.password, env, config, 'DB_PASSWORD', ''),
        'dbname': pick_value(args.dbname, env, config, 'DB_NAME'),
        'sslmode': pick_value(args.sslmode, env, config, 'PG_SSLMODE'),
    }

    missing = [k for k in ('user', 'dbname') if not params[k]]
    if missing:
        print(f'ERROR: missing required params: {', '.join(missing)}', file=sys.stderr)
        sys.exit(2)

    base_conn = None
    for default_db in ('postgres', 'template1'):
        try:
            base_conn = connect_db(params, default_db)
            break
        except psycopg2.Error:
            continue
    if base_conn is None:
        print('ERROR: cannot connect to server with provided credentials', file=sys.stderr)
        sys.exit(3)

    try:
        if not database_exists(base_conn, params['dbname']):
            create_database(base_conn, params['dbname'])
            print(f'База данных '{params['dbname']}' создана')
            target_conn = connect_db(params, params['dbname'])
            try:
                create_schema(target_conn)
                print('Схема создана согласно doc/database.md')
            finally:
                target_conn.close()
            return 0
    finally:
        base_conn.close()

    target_conn = connect_db(params, params['dbname'])
    try:
        create_schema(target_conn)
        migrate_schema(target_conn)
        diffs = compare_schema(target_conn)
        if diffs:
            print('Найдены различия схемы:')
            for item in diffs:
                print(f'- {item}')
            return 4
        print('Схема соответствует описанию')
        return 0
    finally:
        target_conn.close()


if __name__ == '__main__':
    sys.exit(main())
