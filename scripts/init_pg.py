#!/usr/bin/env python3
import argparse
import os
import subprocess
import sys
from urllib.parse import quote, urlencode


def parse_env_file(path):
    data = {}
    if not path:
        return data
    if not os.path.exists(path):
        raise FileNotFoundError(f'Config file not found: {path}')
    with open(path, 'r', encoding='utf-8') as file:
        for raw in file:
            line = raw.strip()
            if not line or line.startswith('#'):
                continue
            if line.startswith('export '):
                line = line[len('export ') :]
            if '=' not in line:
                continue
            key, val = line.split('=', 1)
            data[key.strip()] = val.strip().strip("'").strip('"')
    return data


def pick_value(cli_value, env, config, key, default=None):
    if cli_value is not None:
        return cli_value
    if key in env:
        return env[key]
    if key in config:
        return config[key]
    return default


def normalize_authmode(value):
    return (value or 'password').strip().lower()


def build_database_url(host, port, user, password, dbname, query_params):
    authority = f'{host}:{port}'
    if user:
        user_enc = quote(user, safe='')
        if password:
            password_enc = quote(password, safe='')
            authority = f'{user_enc}:{password_enc}@{authority}'
        else:
            authority = f'{user_enc}@{authority}'
    query = {key: value for key, value in query_params.items() if value}
    query_part = f'?{urlencode(query)}' if query else ''
    return f"postgresql://{authority}/{quote(dbname, safe='')}{query_part}"


def main():
    parser = argparse.ArgumentParser(description='Run PostgreSQL migrations (yoyo).')
    parser.add_argument('--config', help='Path to config file with DB_* variables')
    parser.add_argument('--host')
    parser.add_argument('--port', type=int)
    parser.add_argument('--user')
    parser.add_argument('--password')
    parser.add_argument('--dbname')
    parser.add_argument('--authmode')
    parser.add_argument('--sslmode')
    parser.add_argument('--sslrootcert')
    parser.add_argument('--sslcert')
    parser.add_argument('--sslkey')
    parser.add_argument('--migrations')
    args = parser.parse_args()

    env = os.environ
    config_path = args.config or env.get('DB_CONFIG')
    config = parse_env_file(config_path) if config_path else {}

    host = pick_value(args.host, env, config, 'DB_HOST', 'localhost')
    port = int(pick_value(args.port, env, config, 'DB_PORT', 5432))
    dbname = pick_value(args.dbname, env, config, 'DB_NAME')
    user = pick_value(args.user, env, config, 'DB_USER')
    password = pick_value(args.password, env, config, 'DB_PASSWORD', '')
    authmode = normalize_authmode(pick_value(args.authmode, env, config, 'DB_AUTHMODE', 'password'))
    sslmode = pick_value(args.sslmode, env, config, 'PG_SSLMODE')
    sslrootcert = pick_value(args.sslrootcert, env, config, 'DB_SSL_ROOTCERT')
    sslcert = pick_value(args.sslcert, env, config, 'DB_SSL_CERT')
    sslkey = pick_value(args.sslkey, env, config, 'DB_SSL_KEY')
    migrations = pick_value(
        args.migrations,
        env,
        config,
        'MIGRATIONS_DIR',
        'migrations_broker_pg',
    )

    if not dbname:
        print('ERROR: missing required param: DB_NAME', file=sys.stderr)
        return 2
    if authmode not in {'password', 'ssl'}:
        print(f"ERROR: unsupported DB_AUTHMODE '{authmode}'", file=sys.stderr)
        return 2
    if authmode == 'password' and not user:
        print('ERROR: missing required param for password auth: DB_USER', file=sys.stderr)
        return 2
    if authmode == 'ssl' and not sslmode:
        sslmode = 'verify-full'

    query = {'sslmode': sslmode}
    if authmode == 'ssl':
        query['sslrootcert'] = sslrootcert
        query['sslcert'] = sslcert
        query['sslkey'] = sslkey
        if not user:
            # Cert-auth deployments may map DB user from certificate subject.
            password = ''

    database_url = build_database_url(host, port, user, password, dbname, query)
    command = [
        sys.executable,
        '-m',
        'yoyo',
        'apply',
        '--batch',
        '--database',
        database_url,
        migrations,
    ]
    print(f"Running PostgreSQL migrations: {' '.join(command)}")
    result = subprocess.run(command, check=False)
    return int(result.returncode)


if __name__ == '__main__':
    sys.exit(main())
