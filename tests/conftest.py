from __future__ import annotations

import os
import pathlib
import subprocess
from typing import Callable

import pytest

from tests.utils.process import combined_output


def _repo_root() -> pathlib.Path:
    return pathlib.Path(__file__).resolve().parent.parent


def _build_dir() -> pathlib.Path:
    override = os.getenv('DC_BUILD_DIR')
    if override:
        return pathlib.Path(override).expanduser().resolve()
    return _repo_root() / 'build' / 'x86_64-linux'


def _resolve_binary_path(env_var: str, relative_path: str) -> pathlib.Path:
    override = os.getenv(env_var)
    if override:
        return pathlib.Path(override).expanduser().resolve()
    return (_build_dir() / relative_path).resolve()


def _require_binary(path: pathlib.Path, env_var: str) -> pathlib.Path:
    if path.is_file():
        return path
    pytest.skip(
        f'Binary not found at {path}. Build binaries first or set {env_var} to an executable path.'
    )


@pytest.fixture(scope='session')
def dc_master_bin() -> pathlib.Path:
    path = _resolve_binary_path('DC_MASTER_BIN', 'src/master/dc_master')
    return _require_binary(path, 'DC_MASTER_BIN')


@pytest.fixture(scope='session')
def dc_worker_bin() -> pathlib.Path:
    path = _resolve_binary_path('DC_WORKER_BIN', 'src/worker/dc_worker')
    return _require_binary(path, 'DC_WORKER_BIN')


@pytest.fixture(scope='session')
def dc_cli_bin() -> pathlib.Path:
    path = _resolve_binary_path('DC_CLI_BIN', 'src/cli/dc_cli')
    return _require_binary(path, 'DC_CLI_BIN')


@pytest.fixture(scope='session')
def run_binary() -> Callable[..., subprocess.CompletedProcess[str]]:
    def _run_binary(
        binary: pathlib.Path,
        *args: str,
        timeout: int = 15,
        env: dict[str, str] | None = None,
    ) -> subprocess.CompletedProcess[str]:
        process_env = os.environ.copy()
        if env:
            process_env.update(env)
        return subprocess.run(
            [str(binary), *args],
            capture_output=True,
            text=True,
            timeout=timeout,
            check=False,
            env=process_env,
        )

    return _run_binary


@pytest.fixture(scope='session')
def docker_available() -> None:
    result = subprocess.run(
        ['docker', 'info'],
        capture_output=True,
        text=True,
        timeout=20,
        check=False,
    )
    if result.returncode != 0:
        pytest.skip(
            'Docker daemon is unavailable for smoke tests.\n'
            f'output:\n{combined_output(result.stdout, result.stderr)}'
        )


@pytest.fixture(scope='session')
def docker_tag_prefix() -> str:
    run_id = os.getenv('GITHUB_RUN_ID', 'local')
    run_attempt = os.getenv('GITHUB_RUN_ATTEMPT', '0')
    return f'dc-smoke-{run_id}-{run_attempt}'


@pytest.fixture(scope='session')
def run_command() -> Callable[..., subprocess.CompletedProcess[str]]:
    def _run_command(
        *args: str,
        timeout: int = 120,
        env: dict[str, str] | None = None,
        cwd: pathlib.Path | None = None,
    ) -> subprocess.CompletedProcess[str]:
        process_env = os.environ.copy()
        if env:
            process_env.update(env)
        return subprocess.run(
            [*args],
            capture_output=True,
            text=True,
            timeout=timeout,
            check=False,
            env=process_env,
            cwd=str(cwd) if cwd else None,
        )

    return _run_command
