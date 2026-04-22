from __future__ import annotations

import os
import pathlib
import socket
from typing import Callable

import pytest
import requests

from tests.worker.mock_master_for_worker_server import MockMasterForWorkerServer, WorkerApiRecorder


def _free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(('127.0.0.1', 0))
        return int(sock.getsockname()[1])


@pytest.fixture
def worker_api_recorder() -> WorkerApiRecorder:
    return WorkerApiRecorder()


@pytest.fixture
def worker_master_api_base_url(worker_api_recorder: WorkerApiRecorder) -> str:
    server = MockMasterForWorkerServer('127.0.0.1', _free_port(), worker_api_recorder)
    server.start()
    base_url = f'http://{server.host}:{server.port}'
    response = requests.get(f'{base_url}/health', timeout=2)
    assert response.status_code == 200
    try:
        yield base_url
    finally:
        server.stop()


@pytest.fixture
def run_worker_once(
    dc_worker_bin: pathlib.Path,
    run_binary: Callable[..., object],
    worker_master_api_base_url: str,
    tmp_path: pathlib.Path,
) -> Callable[..., object]:
    def _run_worker_once(agent_id: str, slots: int = 1) -> object:
        log_dir = tmp_path / f'worker-{agent_id}'
        env = os.environ.copy()
        env.update(
            {
                'MASTER_URL': worker_master_api_base_url,
                'AGENT_ID': agent_id,
                'AGENT_OS': 'linux',
                'AGENT_VERSION': '1.0.0',
                'CPU_CORES': '4',
                'RAM_MB': '2048',
                'SLOTS': str(slots),
                'WORKER_LOG_DIR': str(log_dir),
                'WORKER_LOG_FILE': str(log_dir / 'worker.log'),
                'WORKER_LOG_LEVEL': 'debug',
                'CANCEL_CHECK_SEC': '1',
                'UPLOAD_LOGS': 'true',
            }
        )
        return run_binary(dc_worker_bin, '--once', timeout=30, env=env)

    return _run_worker_once
