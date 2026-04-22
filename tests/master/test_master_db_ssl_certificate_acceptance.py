from __future__ import annotations

import os
import re
import shutil
import socket
import time
from pathlib import Path
from typing import Callable

import psycopg2
import pymongo
import pytest
import subprocess

from tests.utils.process import combined_output, start_process, stop_process, wait_for_http_ready


def _require_openssl() -> None:
    if shutil.which('openssl') is None:
        pytest.skip('OpenSSL is required for TLS certificate integration tests.')


def _run_or_fail(
    run_command: Callable[..., subprocess.CompletedProcess[str]],
    *args: str,
    timeout: int = 60,
    cwd: Path | None = None,
) -> subprocess.CompletedProcess[str]:
    result = run_command(*args, timeout=timeout, cwd=cwd)
    if result.returncode != 0:
        pytest.fail(
            f'command failed: {" ".join(args)}\n'
            f'output:\n{combined_output(result.stdout, result.stderr)}',
            pytrace=False,
        )
    return result


def _generate_tls_material(
    run_command: Callable[..., subprocess.CompletedProcess[str]],
    workdir: Path,
) -> dict[str, Path]:
    certs_dir = workdir / 'certs'
    certs_dir.mkdir(parents=True, exist_ok=True)

    _run_or_fail(run_command, 'openssl', 'genrsa', '-out', 'ca.key', '2048', cwd=certs_dir)
    _run_or_fail(
        run_command,
        'openssl',
        'req',
        '-x509',
        '-new',
        '-nodes',
        '-key',
        'ca.key',
        '-sha256',
        '-days',
        '2',
        '-subj',
        '/CN=dc-test-ca',
        '-out',
        'ca.crt',
        cwd=certs_dir,
    )

    _run_or_fail(
        run_command, 'openssl', 'genrsa', '-out', 'server.key', '2048', cwd=certs_dir
    )
    _run_or_fail(
        run_command,
        'openssl',
        'req',
        '-new',
        '-key',
        'server.key',
        '-subj',
        '/CN=localhost',
        '-out',
        'server.csr',
        cwd=certs_dir,
    )
    (certs_dir / 'server.ext').write_text('subjectAltName=DNS:localhost\n', encoding='utf-8')
    _run_or_fail(
        run_command,
        'openssl',
        'x509',
        '-req',
        '-in',
        'server.csr',
        '-CA',
        'ca.crt',
        '-CAkey',
        'ca.key',
        '-CAcreateserial',
        '-out',
        'server.crt',
        '-days',
        '2',
        '-sha256',
        '-extfile',
        'server.ext',
        cwd=certs_dir,
    )

    _run_or_fail(
        run_command, 'openssl', 'genrsa', '-out', 'client.key', '2048', cwd=certs_dir
    )
    _run_or_fail(
        run_command,
        'openssl',
        'req',
        '-new',
        '-key',
        'client.key',
        '-subj',
        '/CN=dc_client',
        '-out',
        'client.csr',
        cwd=certs_dir,
    )
    _run_or_fail(
        run_command,
        'openssl',
        'x509',
        '-req',
        '-in',
        'client.csr',
        '-CA',
        'ca.crt',
        '-CAkey',
        'ca.key',
        '-CAcreateserial',
        '-out',
        'client.crt',
        '-days',
        '2',
        '-sha256',
        cwd=certs_dir,
    )

    server_pem = certs_dir / 'server.pem'
    client_pem = certs_dir / 'client.pem'
    server_pem.write_text(
        (certs_dir / 'server.crt').read_text(encoding='utf-8')
        + (certs_dir / 'server.key').read_text(encoding='utf-8'),
        encoding='utf-8',
    )
    client_pem.write_text(
        (certs_dir / 'client.crt').read_text(encoding='utf-8')
        + (certs_dir / 'client.key').read_text(encoding='utf-8'),
        encoding='utf-8',
    )

    # libpq requires strict private-key permissions.
    os.chmod(certs_dir / 'client.key', 0o600)
    os.chmod(certs_dir / 'server.key', 0o600)

    return {
        'dir': certs_dir,
        'ca_crt': certs_dir / 'ca.crt',
        'server_key': certs_dir / 'server.key',
        'server_crt': certs_dir / 'server.crt',
        'server_pem': server_pem,
        'client_key': certs_dir / 'client.key',
        'client_crt': certs_dir / 'client.crt',
        'client_pem': client_pem,
    }


def _extract_mapped_port(output: str) -> int:
    match = re.search(r':(\d+)\s*$', output.strip())
    if not match:
        raise RuntimeError(f'Could not parse mapped port from docker output: {output!r}')
    return int(match.group(1))


def _docker_logs(
    run_command: Callable[..., subprocess.CompletedProcess[str]], container_id: str
) -> str:
    result = run_command('docker', 'logs', container_id, timeout=20)
    return combined_output(result.stdout, result.stderr)


def _free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(('127.0.0.1', 0))
        return int(sock.getsockname()[1])


def _assert_master_reaches_ready_state(dc_master_bin: Path, env: dict[str, str]) -> None:
    repo_root = Path(__file__).resolve().parents[2]
    process = start_process([str(dc_master_bin)], env=env, cwd=repo_root)
    try:
        url = f"http://127.0.0.1:{env['MASTER_PORT']}/api/v1/does-not-exist"
        status = wait_for_http_ready(url, process, timeout_sec=40, acceptable_statuses={404})
        if status == 404:
            return
        returncode, stdout, stderr = stop_process(process)
        pytest.fail(
            'dc_master did not become ready over TLS DB connection.\n'
            f'returncode={returncode}\n'
            f'output:\n{combined_output(stdout, stderr)}',
            pytrace=False,
        )
    finally:
        stop_process(process)


@pytest.mark.integration
@pytest.mark.docker
def test_postgres_tls_accepts_client_certificates(
    docker_available: None,
    run_command: Callable[..., subprocess.CompletedProcess[str]],
    tmp_path: Path,
) -> None:
    del docker_available
    _require_openssl()

    tls = _generate_tls_material(run_command, tmp_path / 'pg_tls')

    pg_hba = tmp_path / 'pg_hba.conf'
    pg_hba.write_text(
        'local all all trust\n'
        'hostssl all all 0.0.0.0/0 cert clientcert=verify-full\n'
        'hostssl all all ::/0 cert clientcert=verify-full\n',
        encoding='utf-8',
    )
    init_sql = tmp_path / 'init.sql'
    init_sql.write_text(
        'CREATE ROLE dc_client LOGIN;\n'
        'GRANT ALL PRIVILEGES ON DATABASE dc_test TO dc_client;\n',
        encoding='utf-8',
    )

    volume_certs = f'{tls["dir"].resolve().as_posix()}:/certs:ro'
    volume_hba = f'{pg_hba.resolve().as_posix()}:/etc/postgresql/pg_hba.conf:ro'
    volume_init = f'{init_sql.resolve().as_posix()}:/docker-entrypoint-initdb.d/10-cert-user.sql:ro'
    startup_script = (
        'cp /certs/server.crt /tmp/server.crt && '
        'cp /certs/server.key /tmp/server.key && '
        'cp /certs/ca.crt /tmp/ca.crt && '
        'chown postgres:postgres /tmp/server.crt /tmp/server.key /tmp/ca.crt && '
        'chmod 600 /tmp/server.key && '
        'exec docker-entrypoint.sh postgres '
        '-c ssl=on '
        '-c ssl_cert_file=/tmp/server.crt '
        '-c ssl_key_file=/tmp/server.key '
        '-c ssl_ca_file=/tmp/ca.crt '
        '-c hba_file=/etc/postgresql/pg_hba.conf'
    )

    run = _run_or_fail(
        run_command,
        'docker',
        'run',
        '-d',
        '-P',
        '--entrypoint',
        'sh',
        '-e',
        'POSTGRES_PASSWORD=secret',
        '-e',
        'POSTGRES_DB=dc_test',
        '-v',
        volume_certs,
        '-v',
        volume_hba,
        '-v',
        volume_init,
        'postgres:16-alpine',
        '-c',
        startup_script,
        timeout=120,
    )
    container_id = run.stdout.strip()

    try:
        port_output = _run_or_fail(
            run_command, 'docker', 'port', container_id, '5432/tcp', timeout=30
        ).stdout
        host_port = _extract_mapped_port(port_output)

        last_error = ''
        deadline = time.time() + 45
        while time.time() < deadline:
            try:
                conn = psycopg2.connect(
                    host='localhost',
                    port=host_port,
                    dbname='dc_test',
                    user='dc_client',
                    sslmode='verify-full',
                    sslrootcert=str(tls['ca_crt']),
                    sslcert=str(tls['client_crt']),
                    sslkey=str(tls['client_key']),
                    connect_timeout=3,
                )
                with conn:
                    with conn.cursor() as cur:
                        cur.execute('SELECT 1')
                        assert cur.fetchone() == (1,)
                conn.close()
                return
            except Exception as exc:  # pragma: no cover - polling branch
                last_error = str(exc)
                time.sleep(0.5)

        logs = _docker_logs(run_command, container_id)
        pytest.fail(
            'Postgres did not accept certificate-authenticated TLS connection.\n'
            f'last_error={last_error}\n'
            f'container_logs:\n{logs}',
            pytrace=False,
        )
    finally:
        run_command('docker', 'rm', '-f', container_id, timeout=30)


@pytest.mark.integration
@pytest.mark.docker
def test_mongo_tls_accepts_client_certificates(
    docker_available: None,
    run_command: Callable[..., subprocess.CompletedProcess[str]],
    tmp_path: Path,
) -> None:
    del docker_available
    _require_openssl()

    tls = _generate_tls_material(run_command, tmp_path / 'mongo_tls')
    volume_certs = f'{tls["dir"].resolve().as_posix()}:/certs:ro'

    run = _run_or_fail(
        run_command,
        'docker',
        'run',
        '-d',
        '-P',
        '-v',
        volume_certs,
        'mongo:8',
        '--tlsMode',
        'requireTLS',
        '--tlsCertificateKeyFile',
        '/certs/server.pem',
        '--tlsCAFile',
        '/certs/ca.crt',
        '--bind_ip_all',
        timeout=120,
    )
    container_id = run.stdout.strip()

    try:
        port_output = _run_or_fail(
            run_command, 'docker', 'port', container_id, '27017/tcp', timeout=30
        ).stdout
        host_port = _extract_mapped_port(port_output)

        last_error = ''
        deadline = time.time() + 45
        while time.time() < deadline:
            client = None
            try:
                client = pymongo.MongoClient(
                    f'mongodb://localhost:{host_port}/?tls=true&serverSelectionTimeoutMS=3000',
                    tlsCAFile=str(tls['ca_crt']),
                    tlsCertificateKeyFile=str(tls['client_pem']),
                )
                assert client.admin.command('ping')['ok'] == 1.0
                return
            except Exception as exc:  # pragma: no cover - polling branch
                last_error = str(exc)
                time.sleep(0.5)
            finally:
                if client is not None:
                    client.close()

        logs = _docker_logs(run_command, container_id)
        pytest.fail(
            'Mongo did not accept certificate-authenticated TLS connection.\n'
            f'last_error={last_error}\n'
            f'container_logs:\n{logs}',
            pytrace=False,
        )
    finally:
        run_command('docker', 'rm', '-f', container_id, timeout=30)


@pytest.mark.integration
@pytest.mark.docker
def test_master_connects_to_postgres_container_via_tls_certificates(
    docker_available: None,
    dc_master_bin: Path,
    run_command: Callable[..., subprocess.CompletedProcess[str]],
    tmp_path: Path,
) -> None:
    del docker_available
    _require_openssl()

    tls = _generate_tls_material(run_command, tmp_path / 'pg_tls_master')
    pg_hba = tmp_path / 'pg_hba_master.conf'
    pg_hba.write_text(
        'local all all trust\n'
        'hostssl all all 0.0.0.0/0 cert clientcert=verify-full\n'
        'hostssl all all ::/0 cert clientcert=verify-full\n',
        encoding='utf-8',
    )
    init_sql = tmp_path / 'init_master.sql'
    init_sql.write_text(
        'CREATE ROLE dc_client LOGIN;\n'
        'GRANT ALL PRIVILEGES ON DATABASE dc_test TO dc_client;\n',
        encoding='utf-8',
    )

    run = _run_or_fail(
        run_command,
        'docker',
        'run',
        '-d',
        '-P',
        '--entrypoint',
        'sh',
        '-e',
        'POSTGRES_PASSWORD=secret',
        '-e',
        'POSTGRES_DB=dc_test',
        '-v',
        f'{tls["dir"].resolve().as_posix()}:/certs:ro',
        '-v',
        f'{pg_hba.resolve().as_posix()}:/etc/postgresql/pg_hba.conf:ro',
        '-v',
        f'{init_sql.resolve().as_posix()}:/docker-entrypoint-initdb.d/10-cert-user.sql:ro',
        'postgres:16-alpine',
        '-c',
        (
            'cp /certs/server.crt /tmp/server.crt && '
            'cp /certs/server.key /tmp/server.key && '
            'cp /certs/ca.crt /tmp/ca.crt && '
            'chown postgres:postgres /tmp/server.crt /tmp/server.key /tmp/ca.crt && '
            'chmod 600 /tmp/server.key && '
            'exec docker-entrypoint.sh postgres '
            '-c ssl=on '
            '-c ssl_cert_file=/tmp/server.crt '
            '-c ssl_key_file=/tmp/server.key '
            '-c ssl_ca_file=/tmp/ca.crt '
            '-c hba_file=/etc/postgresql/pg_hba.conf'
        ),
        timeout=120,
    )
    container_id = run.stdout.strip()

    try:
        port_output = _run_or_fail(
            run_command, 'docker', 'port', container_id, '5432/tcp', timeout=30
        ).stdout
        host_port = _extract_mapped_port(port_output)

        # Ensure DB is reachable with cert auth before starting master.
        deadline = time.time() + 45
        while time.time() < deadline:
            try:
                conn = psycopg2.connect(
                    host='localhost',
                    port=host_port,
                    dbname='dc_test',
                    user='dc_client',
                    sslmode='verify-full',
                    sslrootcert=str(tls['ca_crt']),
                    sslcert=str(tls['client_crt']),
                    sslkey=str(tls['client_key']),
                    connect_timeout=3,
                )
                conn.close()
                break
            except Exception:
                time.sleep(0.5)
        else:
            pytest.fail(
                'Postgres TLS container was not ready for certificate-authenticated clients.',
                pytrace=False,
            )

        env = os.environ.copy()
        env.update(
            {
                'DB_BACKEND': 'postgres',
                'DB_AUTHMODE': 'ssl',
                'DB_HOST': 'localhost',
                'DB_PORT': str(host_port),
                'DB_NAME': 'dc_test',
                'DB_SSL_ROOTCERT': str(tls['ca_crt']),
                'DB_SSL_CERT': str(tls['client_crt']),
                'DB_SSL_KEY': str(tls['client_key']),
                'PGUSER': 'dc_client',
                'MASTER_HOST': '127.0.0.1',
                'MASTER_PORT': str(_free_port()),
                'MASTER_SKIP_DB_MIGRATION': '1',
            }
        )
        _assert_master_reaches_ready_state(dc_master_bin, env)
    finally:
        run_command('docker', 'rm', '-f', container_id, timeout=30)


@pytest.mark.integration
@pytest.mark.docker
def test_master_connects_to_mongo_container_via_tls_certificates(
    docker_available: None,
    dc_master_bin: Path,
    run_command: Callable[..., subprocess.CompletedProcess[str]],
    tmp_path: Path,
) -> None:
    del docker_available
    _require_openssl()

    tls = _generate_tls_material(run_command, tmp_path / 'mongo_tls_master')
    run = _run_or_fail(
        run_command,
        'docker',
        'run',
        '-d',
        '-P',
        '-v',
        f'{tls["dir"].resolve().as_posix()}:/certs:ro',
        'mongo:8',
        '--tlsMode',
        'requireTLS',
        '--tlsCertificateKeyFile',
        '/certs/server.pem',
        '--tlsCAFile',
        '/certs/ca.crt',
        '--bind_ip_all',
        timeout=120,
    )
    container_id = run.stdout.strip()

    try:
        port_output = _run_or_fail(
            run_command, 'docker', 'port', container_id, '27017/tcp', timeout=30
        ).stdout
        host_port = _extract_mapped_port(port_output)

        # Ensure DB is reachable with TLS certs before starting master.
        deadline = time.time() + 45
        while time.time() < deadline:
            client = None
            try:
                client = pymongo.MongoClient(
                    f'mongodb://localhost:{host_port}/?tls=true&serverSelectionTimeoutMS=3000',
                    tlsCAFile=str(tls['ca_crt']),
                    tlsCertificateKeyFile=str(tls['client_pem']),
                )
                client.admin.command('ping')
                break
            except Exception:
                time.sleep(0.5)
            finally:
                if client is not None:
                    client.close()
        else:
            pytest.fail('Mongo TLS container was not ready for certificate-authenticated clients.')

        env = os.environ.copy()
        env.update(
            {
                'DB_BACKEND': 'mongo',
                'DB_AUTHMODE': 'ssl',
                'DB_HOST': 'localhost',
                'DB_PORT': str(host_port),
                'DB_NAME': 'dc_test',
                'DB_SSL_ROOTCERT': str(tls['ca_crt']),
                'DB_SSL_CERT': str(tls['client_pem']),
                'DB_SSL_KEY': str(tls['client_key']),
                'SSL_CERT_FILE': str(tls['ca_crt']),
                'MASTER_HOST': '127.0.0.1',
                'MASTER_PORT': str(_free_port()),
                'MASTER_SKIP_DB_MIGRATION': '1',
            }
        )
        _assert_master_reaches_ready_state(dc_master_bin, env)
    finally:
        run_command('docker', 'rm', '-f', container_id, timeout=30)
