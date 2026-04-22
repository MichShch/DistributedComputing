from __future__ import annotations

import sys

import pytest
from tests.utils.process import combined_output
from tests.worker.mock_master_for_worker_server import WorkerApiRecorder


@pytest.mark.integration
def test_worker_register_heartbeat_poll_flow(
    run_worker_once,
    worker_api_recorder: WorkerApiRecorder,
) -> None:
    result = run_worker_once('worker-1', slots=2)
    output = combined_output(result.stdout, result.stderr)
    assert result.returncode == 0, output

    tx_names = [tx['tx'] for tx in worker_api_recorder.transactions()]
    assert 'register_agent' in tx_names
    assert 'send_heartbeat' in tx_names
    assert 'poll_tasks' in tx_names


@pytest.mark.integration
def test_worker_task_status_and_log_upload_are_intercepted(
    run_worker_once,
    worker_api_recorder: WorkerApiRecorder,
) -> None:
    worker_api_recorder.enqueue_task_for_agent(
        'worker-2',
        {
            'task_id': 202,
            'command': sys.executable,
            'args': ['-c', 'print(\'ok\')'],
            'env': {},
            'constraints': {},
        },
    )
    result = run_worker_once('worker-2', slots=1)
    output = combined_output(result.stdout, result.stderr)
    assert result.returncode == 0, output

    tx_names = [tx['tx'] for tx in worker_api_recorder.transactions()]
    assert 'update_task_status' in tx_names
    assert 'upload_log' in tx_names
    assert worker_api_recorder.get_task_state(202) == 'succeeded'
    assert 'ok' in worker_api_recorder.get_log(202, 'stdout')
