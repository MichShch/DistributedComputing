from __future__ import annotations

import json
from concurrent.futures import ThreadPoolExecutor, as_completed

import pytest
import requests


def _response_dump(response: requests.Response) -> str:
    try:
        payload = response.json()
        payload_text = json.dumps(payload, ensure_ascii=False, indent=2, sort_keys=True)
    except ValueError:
        payload_text = '<non-json>'
    return (
        f'request={response.request.method} {response.url}\n'
        f'status={response.status_code}\n'
        f'headers={dict(response.headers)}\n'
        f'text={response.text}\n'
        f'json={payload_text}'
    )


def _exercise_agent_api(base_url: str, suffix: str) -> None:
    upsert_payload = {
        'os': 'linux',
        'version': '1.0.0',
        'resources': {'cpu_cores': 4, 'ram_mb': 2048, 'slots': 1},
    }
    response = requests.put(
        f'{base_url}/api/v1/agents/agent-{suffix}',
        json=upsert_payload,
        timeout=3,
    )
    assert response.status_code == 200, _response_dump(response)

    response = requests.get(f'{base_url}/api/v1/agents/agent-{suffix}', timeout=3)
    assert response.status_code == 200, _response_dump(response)
    agent = response.json()['agent']
    assert agent['agent_id'] == f'agent-{suffix}'
    assert agent['status'] == 'idle'


def _exercise_task_api(base_url: str, command_suffix: str) -> None:
    create_payload = {
        'command': '/bin/echo',
        'args': [command_suffix],
        'env': {'K': 'V'},
    }
    response = requests.post(
        f'{base_url}/api/v1/tasks',
        json=create_payload,
        timeout=3,
    )
    assert response.status_code == 201, _response_dump(response)
    task_id = int(response.json()['task_id'])

    response = requests.get(f'{base_url}/api/v1/tasks/{task_id}', timeout=3)
    assert response.status_code == 200, _response_dump(response)
    assert response.json()['task']['command'] == '/bin/echo'

    update_payload = {'state': 'running', 'exit_code': 0}
    response = requests.post(
        f'{base_url}/api/v1/tasks/{task_id}/status',
        json=update_payload,
        timeout=3,
    )
    assert response.status_code == 200, _response_dump(response)

    update_payload = {'state': 'succeeded', 'exit_code': 0}
    response = requests.post(
        f'{base_url}/api/v1/tasks/{task_id}/status',
        json=update_payload,
        timeout=3,
    )
    assert response.status_code == 200, _response_dump(response)

    response = requests.get(f'{base_url}/api/v1/tasks', timeout=3)
    assert response.status_code == 200, _response_dump(response)
    tasks = response.json()['tasks']
    assert any(int(task['task_id']) == task_id for task in tasks), (
        f'created task_id={task_id} not found in tasks list:\n'
        f'{json.dumps(tasks, ensure_ascii=False, indent=2)}'
    )


@pytest.mark.integration
def test_master_agent_api_with_postgres_container(master_api_base_url: str) -> None:
    _exercise_agent_api(master_api_base_url, 'pg')


@pytest.mark.integration
def test_master_task_api_with_postgres_container(master_api_base_url: str) -> None:
    _exercise_task_api(master_api_base_url, 'pg')


@pytest.mark.integration
def test_master_unknown_route_returns_not_found(master_api_base_url: str) -> None:
    response = requests.get(f'{master_api_base_url}/api/v1/does-not-exist', timeout=3)
    assert response.status_code == 404, _response_dump(response)


@pytest.mark.integration
def test_master_agent_api_with_mongo_container(master_api_base_url_mongo: str) -> None:
    _exercise_agent_api(master_api_base_url_mongo, 'mongo')


@pytest.mark.integration
def test_master_task_api_with_mongo_container(master_api_base_url_mongo: str) -> None:
    _exercise_task_api(master_api_base_url_mongo, 'mongo')


@pytest.mark.integration
def test_master_unknown_route_returns_not_found_mongo(master_api_base_url_mongo: str) -> None:
    response = requests.get(f'{master_api_base_url_mongo}/api/v1/does-not-exist', timeout=3)
    assert response.status_code == 404, _response_dump(response)


@pytest.mark.integration
def test_master_mongo_concurrent_poll_update_list_does_not_crash(
    master_api_base_url_mongo: str,
) -> None:
    base_url = master_api_base_url_mongo
    agent_id = 'agent-mongo-concurrency'

    upsert_payload = {
        'os': 'linux',
        'version': '1.0.0',
        'resources': {'cpu_cores': 8, 'ram_mb': 8192, 'slots': 4},
    }
    response = requests.put(
        f'{base_url}/api/v1/agents/{agent_id}',
        json=upsert_payload,
        timeout=11,
    )
    assert response.status_code == 200, _response_dump(response)

    for i in range(12):
        create_payload = {
            'command': '/bin/echo',
            'args': [f'concurrency-{i}'],
            'env': {'K': 'V'},
        }
        response = requests.post(
            f'{base_url}/api/v1/tasks',
            json=create_payload,
            timeout=12,
        )
        assert response.status_code == 201, _response_dump(response)

    def _worker(worker_id: int) -> None:

        response = requests.put(
            f'{base_url}/api/v1/agents/{worker_id}',
            json=upsert_payload,
            timeout=13,
        )
        assert response.status_code == 200, _response_dump(response)

        for i in range(12):

            heartbeat = requests.post(
                f'{base_url}/api/v1/agents/{worker_id}/heartbeat',
                json={'status': 'idle'},
                timeout=14,
            )
            assert heartbeat.status_code == 200, _response_dump(heartbeat)

            poll = requests.post(
                f'{base_url}/api/v1/agents/{worker_id}/tasks:poll',
                json={'free_slots': 2},
                timeout=15,
            )
            assert poll.status_code == 200, _response_dump(poll)

            for task in poll.json().get('tasks', []):
                task_id = task['task_id']
                finish = requests.post(
                    f'{base_url}/api/v1/tasks/{task_id}/status',
                    json={
                        'state': 'succeeded',
                        'exit_code': 0,
                        },
                    timeout=16,
                )
                assert finish.status_code in (200, 409), _response_dump(finish)

            listed = requests.get(f'{base_url}/api/v1/tasks', timeout=17)
            assert listed.status_code == 200, _response_dump(listed)

            if i % 4 == 0:
                created = requests.post(
                    f'{base_url}/api/v1/tasks',
                    json={'command': '/bin/echo', 'args': [f'w{worker_id}-{i}']},
                    timeout=18,
                )
                assert created.status_code == 201, _response_dump(created)

    with ThreadPoolExecutor(max_workers=4) as executor:
        futures = [executor.submit(_worker, i+1) for i in range(4)]
        for future in as_completed(futures):
            future.result()

    response = requests.get(f'{base_url}/api/v1/tasks', timeout=19)
    assert response.status_code == 200, _response_dump(response)
