from __future__ import annotations

import os
import time
from pathlib import Path

import pytest

from tests.utils.process import combined_output


@pytest.mark.integration
def test_master_rejects_unknown_db_backend(dc_master_bin, run_binary) -> None:
    env = os.environ.copy()
    env['DB_BACKEND'] = 'invalid-backend'
    result = run_binary(dc_master_bin, env=env)
    output = combined_output(result.stdout, result.stderr)

    assert result.returncode != 0
    assert 'Invalid DB_BACKEND' in output


@pytest.mark.integration
def test_master_mongo_backend_requires_connection_env(dc_master_bin, run_binary) -> None:
    env = os.environ.copy()
    env['DB_BACKEND'] = 'mongo'
    env.pop('DB_HOST', None)
    env.pop('DB_PORT', None)
    env.pop('DB_USER', None)
    env.pop('DB_PASSWORD', None)
    env.pop('DB_NAME', None)
    result = run_binary(dc_master_bin, env=env)
    output = combined_output(result.stdout, result.stderr)

    assert result.returncode != 0
    assert 'Missing user or password for password authentification' in output


@pytest.mark.integration
def test_master_runs_mongo_migration_script_on_mongo_backend(
    dc_master_bin, run_binary, tmp_path: Path
) -> None:
    mongo_script = tmp_path / 'mongo_init_fail.py'
    mongo_script.write_text(
        'import os\n'
        'import sys\n'
        'print(\'mongo migration script invoked\')\n'
        'print(f\"mongo auth source={os.getenv(\'DB_MONGO_AUTH_SOURCE\', \'\')}\")\n'
        'sys.exit(73)\n',
        encoding='utf-8',
    )

    env = os.environ.copy()
    env['DB_BACKEND'] = 'mongo'
    env['DB_HOST'] = '127.0.0.1'
    env['DB_PORT'] = '27017'
    env['DB_USER'] = 'local'
    env['DB_PASSWORD'] = 'local'
    env['DB_NAME'] = 'dc_test'
    env['DB_MONGO_AUTH_SOURCE'] = 'admin'
    env['INIT_DB_SCRIPT'] = str(mongo_script)
    
    result = run_binary(dc_master_bin, env=env)
    output = combined_output(result.stdout, result.stderr)

    assert result.returncode == 73
    assert 'mongo migration script invoked' in output, output
    assert 'mongo auth source=admin' in output, output
    assert 'Migrations failed with code 73' in output, output


@pytest.mark.integration
def test_master_postgres_backend_requires_credentials(dc_master_bin, run_binary) -> None:
    env = os.environ.copy()
    env['DB_BACKEND'] = 'postgres'
    env.pop('DB_USER', None)
    env.pop('DB_NAME', None)
    result = run_binary(dc_master_bin, env=env)
    output = combined_output(result.stdout, result.stderr)

    assert result.returncode != 0
    assert 'Missing user or password for password authentification' in output


@pytest.mark.integration
@pytest.mark.parametrize(
    'missing_ssl_var',
    [
        'DB_SSL_ROOTCERT',
        'DB_SSL_CERT',
        'DB_SSL_KEY',
    ],
)
def test_master_ssl_auth_requires_all_ssl_certificate_env_vars(
    dc_master_bin, run_binary, missing_ssl_var: str
) -> None:
    env = os.environ.copy()
    env['DB_BACKEND'] = 'postgres'
    env['DB_AUTHMODE'] = 'ssl'
    env['DB_NAME'] = 'dc_test'
    env['DB_SSL_ROOTCERT'] = '/tmp/root.crt'
    env['DB_SSL_CERT'] = '/tmp/client.crt'
    env['DB_SSL_KEY'] = '/tmp/client.key'
    env.pop(missing_ssl_var, None)

    result = run_binary(dc_master_bin, env=env)
    output = combined_output(result.stdout, result.stderr)

    assert result.returncode != 0
    assert 'Missing rootcert, cert or user for key for ssl authentification' in output


@pytest.mark.integration
def test_master_accepts_ssl_auth_mode_and_reaches_migration_step(
    dc_master_bin, run_binary, tmp_path: Path
) -> None:
    ssl_script = tmp_path / 'ssl_migrate_fail.py'
    ssl_script.write_text(
        'import sys\n'
        'print(\'ssl migration script invoked\')\n'
        'sys.exit(77)\n',
        encoding='utf-8',
    )

    env = os.environ.copy()
    env['DB_BACKEND'] = 'postgres'
    env['DB_AUTHMODE'] = 'ssl'
    env['DB_HOST'] = '127.0.0.1'
    env['DB_PORT'] = '5432'
    env['DB_NAME'] = 'dc_test'
    env['DB_SSL_ROOTCERT'] = '/tmp/root.crt'
    env['DB_SSL_CERT'] = '/tmp/client.crt'
    env['DB_SSL_KEY'] = '/tmp/client.key'
    env['INIT_DB_SCRIPT'] = str(ssl_script)

    result = run_binary(dc_master_bin, env=env)
    output = combined_output(result.stdout, result.stderr)

    assert result.returncode == 77
    assert 'ssl migration script invoked' in output, output
    assert 'Migrations failed with code 77' in output, output


@pytest.mark.integration
def test_master_postgres_ssl_startup_fails_with_invalid_tls_material(dc_master_bin, run_binary) -> None:
    env = os.environ.copy()
    env['DB_BACKEND'] = 'postgres'
    env['DB_AUTHMODE'] = 'ssl'
    env['DB_HOST'] = '127.0.0.1'
    env['DB_PORT'] = '5432'
    env['DB_NAME'] = 'dc_test'
    env['DB_SSL_ROOTCERT'] = '/does/not/exist/root.crt'
    env['DB_SSL_CERT'] = '/does/not/exist/client.crt'
    env['DB_SSL_KEY'] = '/does/not/exist/client.key'
    env['MASTER_SKIP_DB_MIGRATION'] = '1'
    env['BROKER_RECONNECT_ATTEMPTS'] = '1'
    env['BROKER_RECONNECT_COOLDOWN_SEC'] = '0'

    result = run_binary(dc_master_bin, env=env)
    output = combined_output(result.stdout, result.stderr)

    assert result.returncode != 0
    assert "Broker operation 'postgres startup connect' failed on final attempt 1/1" in output


@pytest.mark.integration
def test_master_runs_postgres_migration_script_on_postgres_backend(
    dc_master_bin, run_binary, tmp_path: Path
) -> None:
    pg_script = tmp_path / 'pg_migrate_fail.py'
    pg_script.write_text(
        'import sys\n'
        'print(\'postgres migration script invoked\')\n'
        'sys.exit(74)\n',
        encoding='utf-8',
    )

    env = os.environ.copy()
    env['DB_BACKEND'] = 'postgres'
    env['DB_HOST'] = '127.0.0.1'
    env['DB_PORT'] = '5432'
    env['DB_USER'] = 'postgres'
    env['DB_PASSWORD'] = 'secret'
    env['DB_NAME'] = 'dc_test'
    env['INIT_DB_SCRIPT'] = str(pg_script)
    result = run_binary(dc_master_bin, env=env)
    output = combined_output(result.stdout, result.stderr)

    assert result.returncode == 74
    assert 'postgres migration script invoked' in output, output
    assert 'Migrations failed with code 74' in output, output


@pytest.mark.integration
def test_master_skips_migrations_when_flag_enabled(
    dc_master_bin, run_binary, tmp_path: Path
) -> None:
    pg_script = tmp_path / 'pg_should_not_run.py'
    pg_script.write_text(
        'import sys\n'
        'print(\'postgres migration script invoked\')\n'
        'sys.exit(74)\n',
        encoding='utf-8',
    )

    env = os.environ.copy()
    env['DB_BACKEND'] = 'postgres'
    env['DB_HOST'] = '127.0.0.1'
    env['DB_PORT'] = '5432'
    env['DB_USER'] = 'postgres'
    env['DB_PASSWORD'] = 'secret'
    env['DB_NAME'] = 'dc_test'
    env['INIT_DB_SCRIPT'] = str(pg_script)
    env['MASTER_SKIP_DB_MIGRATION'] = '1'
    env['MASTER_HOST'] = '256.256.256.256'
    result = run_binary(dc_master_bin, env=env)
    output = combined_output(result.stdout, result.stderr)

    assert result.returncode != 74
    assert 'postgres migration script invoked' not in output
    assert 'DB migration step skipped' in output


@pytest.mark.integration
def test_master_rejects_non_integer_broker_reconnect_attempts(dc_master_bin, run_binary) -> None:
    env = os.environ.copy()
    env['BROKER_RECONNECT_ATTEMPTS'] = 'abc'
    result = run_binary(dc_master_bin, env=env)
    output = combined_output(result.stdout, result.stderr)

    assert result.returncode != 0
    assert "Invalid BROKER_RECONNECT_ATTEMPTS: expected integer, got 'abc'" in output


@pytest.mark.integration
def test_master_rejects_negative_broker_reconnect_cooldown(dc_master_bin, run_binary) -> None:
    env = os.environ.copy()
    env['BROKER_RECONNECT_COOLDOWN_SEC'] = '-1'
    result = run_binary(dc_master_bin, env=env)
    output = combined_output(result.stdout, result.stderr)

    assert result.returncode != 0
    assert 'Invalid BROKER_RECONNECT_COOLDOWN_SEC: must be >= 0, got -1' in output


@pytest.mark.integration
def test_master_applies_broker_reconnect_attempts_and_cooldown_on_startup_failure(
    dc_master_bin, run_binary
) -> None:
    env = os.environ.copy()
    env.update(
        {
            'DB_BACKEND': 'mongo',
            'DB_HOST': '127.0.0.1',
            'DB_PORT': '1',
            'DB_USER': 'u',
            'DB_PASSWORD': 'p',
            'DB_NAME': 'd',
            'MASTER_SKIP_DB_MIGRATION': '1',
            'BROKER_RECONNECT_ATTEMPTS': '2',
            'BROKER_RECONNECT_COOLDOWN_SEC': '1',
        }
    )

    started = time.monotonic()
    result = run_binary(dc_master_bin, env=env, timeout=8)
    elapsed = time.monotonic() - started
    output = combined_output(result.stdout, result.stderr)

    assert result.returncode != 0
    assert "Broker operation 'mongo startup ping' failed on attempt 1/2" in output
    assert "Broker operation 'mongo startup ping' failed on final attempt 2/2" in output
    assert elapsed >= 0.8, f'expected reconnect cooldown delay, elapsed={elapsed:.3f}s'
