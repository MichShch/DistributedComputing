from __future__ import annotations

import os
import pathlib
import socket
import sys

import pytest
from testcontainers.mongodb import MongoDbContainer
from testcontainers.postgres import PostgresContainer

from tests.utils.process import (
    ManagedProcess,
    combined_output,
    start_process,
    stop_process,
    wait_for_http_ready,
)


def _free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(('127.0.0.1', 0))
        return int(sock.getsockname()[1])


def _venv_python(repo_root: pathlib.Path) -> str:
    if sys.platform == 'win32':
        candidate = repo_root / '.venv' / 'Scripts' / 'python.exe'
    else:
        candidate = repo_root / '.venv' / 'bin' / 'python3'
    if not candidate.is_file():
        pytest.fail(
            f'Expected tests to use .venv Python, but it was not found: {candidate}',
            pytrace=False,
        )
    return str(candidate)


def _slug(text: str) -> str:
    cleaned = ''.join(ch if ch.isalnum() or ch in {'-', '_', '.'} else '_' for ch in text)
    return cleaned or 'test'


def _venv_binary(repo_root: pathlib.Path, name: str) -> str:
    if sys.platform == 'win32':
        candidate = repo_root / '.venv' / 'Scripts' / f'{name}.exe'
    else:
        candidate = repo_root / '.venv' / 'bin' / name
    if not candidate.is_file():
        pytest.fail(
            f'Expected tests to use .venv binary, but it was not found: {candidate}',
            pytrace=False,
        )
    return str(candidate)


def _write_mongo_migrations_config(
    repo_root: pathlib.Path,
    username: str,
    password: str,
) -> pathlib.Path:
    logs_dir = repo_root / 'logs' / 'mongo'
    logs_dir.mkdir(parents=True, exist_ok=True)
    config_path = logs_dir / 'mongodb-migrations.ini'
    config_path.write_text(
        '[mongo]\n'
        'host=127.0.0.1\n'
        'port=27017\n'
        'database=dc_test\n'
        'migrations=migrations_broker_mongo\n'
        'metastore=database_migrations\n'
        'dry_run=false\n'
        f'username={username}\n'
        f'password={password}\n',
        encoding='utf-8',
    )
    return config_path


def _read_container_logs(container: object, container_name: str) -> str:
    # testcontainers API may return bytes or (stdout, stderr)
    try:
        raw = container.get_logs()  # type: ignore[attr-defined]
        if isinstance(raw, tuple):
            chunks: list[str] = []
            for part in raw:
                if isinstance(part, bytes):
                    chunks.append(part.decode('utf-8', errors='replace'))
                else:
                    chunks.append(str(part))
            return '\n'.join(chunks).strip()
        if isinstance(raw, bytes):
            return raw.decode('utf-8', errors='replace')
        return str(raw)
    except Exception:
        pass

    try:
        docker_obj = getattr(container, '_container', None)
        if docker_obj is not None:
            raw2 = docker_obj.logs(stdout=True, stderr=True, timestamps=True)
            if isinstance(raw2, bytes):
                return raw2.decode('utf-8', errors='replace')
            return str(raw2)
    except Exception as exc:  # pragma: no cover
        return f'<failed to fetch {container_name} logs: {exc}>'

    return f'<{container_name} logs unavailable>'


def _dump_container_logs(
    container: object, nodeid: str, repo_root: pathlib.Path, phase: str, backend: str
) -> None:
    logs_dir = repo_root / 'logs' / backend
    logs_dir.mkdir(parents=True, exist_ok=True)
    out_file = logs_dir / f'{_slug(nodeid)}-{_slug(phase)}.log'
    out_file.write_text(_read_container_logs(container, backend), encoding='utf-8')


@pytest.fixture
def postgres_container(request: pytest.FixtureRequest) -> PostgresContainer:
    repo_root = pathlib.Path(__file__).resolve().parents[2]
    container = PostgresContainer(
        'postgres:16-alpine',
        username='postgres',
        password='secret',
        dbname='dc_test',
    )
    try:
        container.start()
    except Exception as exc:  # pragma: no cover - environment dependent
        pytest.skip(f'Postgres container is unavailable: {exc}')
    _dump_container_logs(container, request.node.nodeid, repo_root, 'after-init', 'postgres')
    try:
        yield container
    finally:
        _dump_container_logs(container, request.node.nodeid, repo_root, 'teardown', 'postgres')
        container.stop()


@pytest.fixture
def mongo_container(request: pytest.FixtureRequest) -> MongoDbContainer:
    repo_root = pathlib.Path(__file__).resolve().parents[2]
    container = MongoDbContainer('mongo:8')
    try:
        container.start()
    except Exception as exc:  # pragma: no cover - environment dependent
        pytest.skip(f'Mongo container is unavailable: {exc}')
    _dump_container_logs(container, request.node.nodeid, repo_root, 'after-init', 'mongo')
    try:
        yield container
    finally:
        _dump_container_logs(container, request.node.nodeid, repo_root, 'teardown', 'mongo')
        container.stop()


@pytest.fixture
def master_api_base_url(
    dc_master_bin: pathlib.Path,
    postgres_container: PostgresContainer,
) -> str:
    port = _free_port()
    repo_root = pathlib.Path(__file__).resolve().parents[2]
    migration_python = _venv_python(repo_root)
    log_dir = repo_root / 'logs'
    log_dir.mkdir(parents=True, exist_ok=True)
    db_host = postgres_container.get_container_host_ip()
    db_port = postgres_container.get_exposed_port(5432)
    env = os.environ.copy()
    env.update(
        {
            'DB_BACKEND': 'postgres',
            'DB_HOST': db_host,
            'DB_PORT': str(db_port),
            'DB_USER': 'postgres',
            'DB_PASSWORD': 'secret',
            'DB_NAME': 'dc_test',
            'MASTER_HOST': '127.0.0.1',
            'MASTER_PORT': str(port),
            'LOG_DIR': str(log_dir),
            'MASTER_LOG_FILE': str(log_dir / 'master.log'),
            'INIT_DB_PYTHON': migration_python,
            'INIT_DB_SCRIPT': str(repo_root / 'scripts' / 'init_pg.py'),
            'MIGRATIONS_DIR': str(repo_root / 'migrations_broker_pg'),
            'MASTER_SKIP_DB_MIGRATION': 'false',
        }
    )
    process: ManagedProcess = start_process([str(dc_master_bin)], env=env, cwd=repo_root)
    base_url = f'http://127.0.0.1:{port}'
    status = wait_for_http_ready(
        f'{base_url}/api/v1/tasks',
        process,
        timeout_sec=30,
        acceptable_statuses={200},
    )
    if status == -1:
        returncode, stdout, stderr = stop_process(process)
        pytest.fail(
            'dc_master did not become ready.\n'
            f'returncode={returncode}\n'
            f'output:\n{combined_output(stdout, stderr)}'
        )
    try:
        yield base_url
    finally:
        stop_process(process)


@pytest.fixture
def master_api_base_url_mongo(
    dc_master_bin: pathlib.Path,
    mongo_container: MongoDbContainer,
) -> str:
    port = _free_port()
    repo_root = pathlib.Path(__file__).resolve().parents[2]
    migration_python = _venv_python(repo_root)
    log_dir = repo_root / 'logs'
    log_dir.mkdir(parents=True, exist_ok=True)
    mongo_username = str(getattr(mongo_container, 'username', 'test'))
    mongo_password = str(getattr(mongo_container, 'password', 'test'))
    db_host = mongo_container.get_container_host_ip()
    db_port = mongo_container.get_exposed_port(27017)
    migration_config = _write_mongo_migrations_config(
        repo_root=repo_root,
        username=mongo_username,
        password=mongo_password,
    )
    env = os.environ.copy()
    env.update(
        {
            'DB_BACKEND' : 'mongo',
            'DB_HOST'    : db_host,
            'DB_PORT'    : str(db_port),
            'DB_USER'    : mongo_username,
            'DB_PASSWORD': mongo_password,
            'DB_NAME'    : 'dc_test',
            'DB_MONGO_AUTH_SOURCE': 'admin',
            'MASTER_HOST': '127.0.0.1',
            'MASTER_PORT': str(port),
            'LOG_DIR': str(log_dir),
            'MASTER_LOG_FILE': str(log_dir / 'master.log'),
            'INIT_DB_PYTHON': migration_python,
            'INIT_DB_SCRIPT': str(repo_root / 'scripts' / 'init_mongo.py'),
            'MIGRATIONS_DIR': str(repo_root / 'migrations_broker_mongo'),
            'MONGODB_MIGRATIONS_CONFIG': str(migration_config),
            'MASTER_SKIP_DB_MIGRATION': 'true',
        }
    )
    process: ManagedProcess = start_process([str(dc_master_bin)], env=env, cwd=repo_root)
    base_url = f'http://127.0.0.1:{port}'
    status = wait_for_http_ready(
        f'{base_url}/api/v1/tasks',
        process,
        timeout_sec=30,
        acceptable_statuses={200},
    )
    if status == -1:
        returncode, stdout, stderr = stop_process(process)
        pytest.fail(
            'dc_master did not become ready on mongo backend.\n'
            f'returncode={returncode}\n'
            f'output:\n{combined_output(stdout, stderr)}'
        )
    try:
        yield base_url
    finally:
        stop_process(process)
