import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from urllib.parse import quote_plus, urlencode


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
            data[key.strip()] = val.strip().strip('\'').strip('\"')
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


def make_combined_pem_file(cert_path, key_path):
    cert_path = cert_path.strip()
    key_path = key_path.strip()
    if cert_path == key_path:
        return cert_path, False

    with open(cert_path, 'r', encoding='utf-8') as cert_file:
        cert_data = cert_file.read().rstrip() + '\n'
    with open(key_path, 'r', encoding='utf-8') as key_file:
        key_data = key_file.read().rstrip() + '\n'

    fd, combined_path = tempfile.mkstemp(prefix='mongo_tls_', suffix='.pem')
    os.close(fd)
    with open(combined_path, 'w', encoding='utf-8') as out:
        out.write(cert_data)
        out.write(key_data)
    return combined_path, True


def build_mongo_url(
    host,
    port,
    user,
    password,
    authmode,
    ssl_rootcert=None,
    ssl_cert=None,
    ssl_key=None,
    auth_source='admin',
):
    auth = ''
    query = {}
    temp_files = []
    if authmode == 'password':
        if user:
            user_enc = quote_plus(user)
            if password:
                auth = f'{user_enc}:{quote_plus(password)}@'
            else:
                auth = f'{user_enc}@'
            query['authSource'] = auth_source
    elif authmode == 'ssl':
        query['tls'] = 'true'
        if ssl_rootcert:
            query['tlsCAFile'] = ssl_rootcert
        if ssl_cert and ssl_key:
            cert_key_file, is_temp = make_combined_pem_file(ssl_cert, ssl_key)
            query['tlsCertificateKeyFile'] = cert_key_file
            if is_temp:
                temp_files.append(cert_key_file)
        elif ssl_cert:
            query['tlsCertificateKeyFile'] = ssl_cert
    query_part = f'?{urlencode(query)}' if query else ''
    return f'mongodb://{auth}{host}:{port}/{query_part}', temp_files


def build_command(binary, url, database, username, password, migrations, metastore):
    command = [
        binary,
        '--url',
        url,
        '--database',
        database,
        '--migrations',
        migrations,
        '--metastore',
        metastore,
    ]
    if username is not None:
        command.extend(['--username', username])
    if password is not None:
        command.extend(['--password', password])
    return command


def run_command(command):
    print(f"Running Mongo migrations: {' '.join(command)}")
    result = subprocess.run(command, check=False)
    return int(result.returncode)


def main():
    parser = argparse.ArgumentParser(description='Run MongoDB migrations.')
    parser.add_argument('--config', help='Path to config file with MONGO_* variables')
    parser.add_argument('--url')
    parser.add_argument('--host')
    parser.add_argument('--port')
    parser.add_argument('--user')
    parser.add_argument('--password')
    parser.add_argument('--authmode')
    parser.add_argument('--authsource')
    parser.add_argument('--sslrootcert')
    parser.add_argument('--sslcert')
    parser.add_argument('--sslkey')
    parser.add_argument('--database')
    parser.add_argument('--migrations')
    parser.add_argument('--metastore')
    parser.add_argument('--binary')
    args = parser.parse_args()

    env = os.environ
    config_path = args.config or env.get('DB_CONFIG')
    config = parse_env_file(config_path) if config_path else {}

    db_host = pick_value(args.host, env, config, 'DB_HOST', 'localhost')
    db_port = pick_value(args.port, env, config, 'DB_PORT', '27017')
    db_user = pick_value(args.user, env, config, 'DB_USER')
    db_password = pick_value(args.password, env, config, 'DB_PASSWORD')
    authmode = normalize_authmode(pick_value(args.authmode, env, config, 'DB_AUTHMODE', 'password'))
    auth_source = pick_value(args.authsource, env, config, 'DB_MONGO_AUTH_SOURCE', 'admin')
    ssl_rootcert = pick_value(args.sslrootcert, env, config, 'DB_SSL_ROOTCERT')
    ssl_cert = pick_value(args.sslcert, env, config, 'DB_SSL_CERT')
    ssl_key = pick_value(args.sslkey, env, config, 'DB_SSL_KEY')
    temporary_files = []
    if args.url:
        mongo_url = args.url
    else:
        mongo_url, temporary_files = build_mongo_url(
            db_host,
            db_port,
            db_user,
            db_password,
            authmode,
            ssl_rootcert=ssl_rootcert,
            ssl_cert=ssl_cert,
            ssl_key=ssl_key,
            auth_source=auth_source,
        )
    db_name = pick_value(args.database, env, config, 'DB_NAME')
    migrations = pick_value(
        args.migrations,
        env,
        config,
        'MIGRATIONS_DIR',
        'migrations_broker_mongo',
    )
    metastore = pick_value(
        args.metastore,
        env,
        config,
        'MONGO_MIGRATIONS_METASTORE',
        'database_migrations',
    )

    if authmode not in {'password', 'ssl'}:
        print(f"ERROR: unsupported DB_AUTHMODE '{authmode}'", file=sys.stderr)
        return 2

    if not db_name:
        print('ERROR: missing required param: DB_NAME', file=sys.stderr)
        return 2

    if not args.url and (not db_host or not db_port):
        print('ERROR: missing required params: DB_HOST and/or DB_PORT', file=sys.stderr)
        return 2

    if authmode == 'password' and (not db_user or db_password is None):
        print('ERROR: missing required params for password auth: DB_USER and/or DB_PASSWORD', file=sys.stderr)
        return 2

    if authmode == 'ssl' and not ssl_cert and not ssl_key and not ssl_rootcert:
        print(
            'ERROR: SSL auth requested but DB_SSL_ROOTCERT/DB_SSL_CERT/DB_SSL_KEY are empty',
            file=sys.stderr,
        )
        return 2

    binaries = [args.binary] if args.binary else ['mongodb-migrate', 'mongodb-migrations']
    cli_user = db_user if authmode == 'password' else None
    cli_password = db_password if authmode == 'password' else None

    for binary in binaries:
        resolved = shutil.which(binary)
        if not resolved:
            continue
        try:
            return run_command(
                build_command(
                    resolved,
                    mongo_url,
                    db_name,
                    cli_user,
                    cli_password,
                    migrations,
                    metastore,
                )
            )
        finally:
            for file_path in temporary_files:
                try:
                    os.remove(file_path)
                except OSError:
                    pass

    print(
        'ERROR: mongodb-migrations executable not found. '
        'Install dependencies and ensure \'mongodb-migrate\' is in PATH.',
        file=sys.stderr,
    )
    return 127


if __name__ == '__main__':
    sys.exit(main())
