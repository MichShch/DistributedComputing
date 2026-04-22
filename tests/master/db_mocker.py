from __future__ import annotations

from dataclasses import dataclass, field
from datetime import datetime, timezone
from threading import Lock
from typing import Any


def _utc_now() -> str:
    return datetime.now(timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')


@dataclass
class MockDbState:
    next_task_id: int = 1
    agents: dict[str, dict[str, Any]] = field(default_factory=dict)
    tasks: dict[int, dict[str, Any]] = field(default_factory=dict)
    task_order: list[int] = field(default_factory=list)
    transactions: list[dict[str, Any]] = field(default_factory=list)


class DbMocker:
    '''In-memory DB mock that records transaction-like operations.'''

    def __init__(self) -> None:
        self._state = MockDbState()
        self._lock = Lock()

    def _record(self, tx: str, payload: dict[str, Any]) -> None:
        self._state.transactions.append(
            {
                'tx': tx,
                'payload': payload,
                'at': _utc_now(),
            }
        )

    def upsert_agent(self, agent_id: str, body: dict[str, Any]) -> None:
        with self._lock:
            self._record('upsert_agent', {'agent_id': agent_id, 'body': body})
            current = self._state.agents.get(agent_id, {})
            current.update(
                {
                    'agent_id': agent_id,
                    'os': body.get('os', ''),
                    'version': body.get('version', ''),
                    'resources': body.get('resources', {}),
                    'status': 'idle',
                    'last_heartbeat': _utc_now(),
                }
            )
            self._state.agents[agent_id] = current

    def get_agent(self, agent_id: str) -> dict[str, Any] | None:
        with self._lock:
            self._record('get_agent', {'agent_id': agent_id})
            agent = self._state.agents.get(agent_id)
            return dict(agent) if agent else None

    def list_agents(self) -> list[dict[str, Any]]:
        with self._lock:
            self._record('list_agents', {})
            return [dict(v) for _, v in sorted(self._state.agents.items())]

    def create_task(self, body: dict[str, Any]) -> int:
        with self._lock:
            task_id = self._state.next_task_id
            self._state.next_task_id += 1
            task = {
                'task_id': task_id,
                'state': 'queued',
                'command': body['command'],
                'args': body.get('args', []),
                'env': body.get('env', {}),
                'timeout_sec': body.get('timeout_sec'),
                'constraints': body.get('constraints', {}),
                'created_at': _utc_now(),
                'started_at': None,
                'finished_at': None,
                'exit_code': None,
                'error_message': None,
                'assigned_agent': None,
            }
            self._record('create_task', {'task': task})
            self._state.tasks[task_id] = task
            self._state.task_order.append(task_id)
            return task_id

    def get_task(self, task_id: int) -> dict[str, Any] | None:
        with self._lock:
            self._record('get_task', {'task_id': task_id})
            task = self._state.tasks.get(task_id)
            return dict(task) if task else None

    def list_tasks(self) -> list[dict[str, Any]]:
        with self._lock:
            self._record('list_tasks', {})
            out: list[dict[str, Any]] = []
            for task_id in reversed(self._state.task_order):
                task = self._state.tasks[task_id]
                out.append({'task_id': task['task_id'], 'state': task['state']})
            return out

    def update_task_status(self, task_id: int, body: dict[str, Any]) -> bool:
        with self._lock:
            task = self._state.tasks.get(task_id)
            self._record('update_task_status', {'task_id': task_id, 'body': body})
            if not task:
                return False
            task['state'] = body['state']
            if 'exit_code' in body:
                task['exit_code'] = body['exit_code']
            if 'error_message' in body:
                task['error_message'] = body['error_message']
            if body['state'] == 'running' and not task['started_at']:
                task['started_at'] = _utc_now()
            if body['state'] in {'succeeded', 'failed', 'canceled'}:
                task['finished_at'] = _utc_now()
            return True

    def transactions(self) -> list[dict[str, Any]]:
        with self._lock:
            return [dict(tx) for tx in self._state.transactions]
