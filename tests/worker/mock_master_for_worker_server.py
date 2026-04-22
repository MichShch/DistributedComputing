from __future__ import annotations

import json
import threading
from dataclasses import dataclass, field
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any
from urllib.parse import urlparse


def _utc_now() -> str:
    return datetime.now(timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')


def _json_response(handler: BaseHTTPRequestHandler, status: int, payload: dict[str, Any]) -> None:
    raw = json.dumps(payload).encode('utf-8')
    handler.send_response(status)
    handler.send_header('Content-Type', 'application/json; charset=utf-8')
    handler.send_header('Content-Length', str(len(raw)))
    handler.end_headers()
    handler.wfile.write(raw)


@dataclass
class RecorderState:
    agents: dict[str, dict[str, Any]] = field(default_factory=dict)
    task_states: dict[int, str] = field(default_factory=dict)
    queued_tasks_by_agent: dict[str, list[dict[str, Any]]] = field(default_factory=dict)
    logs: dict[int, dict[str, str]] = field(default_factory=dict)
    transactions: list[dict[str, Any]] = field(default_factory=list)


class WorkerApiRecorder:
    def __init__(self) -> None:
        self._state = RecorderState()
        self._lock = threading.Lock()

    def _record(self, tx: str, payload: dict[str, Any]) -> None:
        self._state.transactions.append({'tx': tx, 'payload': payload, 'at': _utc_now()})

    def register_agent(self, agent_id: str, body: dict[str, Any]) -> dict[str, Any]:
        with self._lock:
            self._record('register_agent', {'agent_id': agent_id, 'body': body})
            self._state.agents[agent_id] = {
                'agent_id': agent_id,
                'os': body.get('os', ''),
                'version': body.get('version', ''),
                'resources': body.get('resources', {}),
                'status': 'idle',
                'last_heartbeat': _utc_now(),
            }
            self._state.queued_tasks_by_agent.setdefault(agent_id, [])
            return {'status': 'ok', 'heartbeat_interval_sec': 5}

    def send_heartbeat(self, agent_id: str, body: dict[str, Any]) -> bool:
        with self._lock:
            self._record('send_heartbeat', {'agent_id': agent_id, 'body': body})
            if agent_id not in self._state.agents:
                return False
            self._state.agents[agent_id]['status'] = body.get('status', 'idle')
            self._state.agents[agent_id]['last_heartbeat'] = _utc_now()
            return True

    def enqueue_task_for_agent(self, agent_id: str, task: dict[str, Any]) -> None:
        with self._lock:
            self._record('enqueue_task_for_agent', {'agent_id': agent_id, 'task': task})
            self._state.queued_tasks_by_agent.setdefault(agent_id, []).append(task)
            if 'task_id' in task:
                self._state.task_states[int(task['task_id'])] = 'queued'

    def poll_tasks(self, agent_id: str, free_slots: int) -> tuple[bool, list[dict[str, Any]]]:
        with self._lock:
            self._record('poll_tasks', {'agent_id': agent_id, 'free_slots': free_slots})
            if agent_id not in self._state.agents:
                return False, []
            queue = self._state.queued_tasks_by_agent.setdefault(agent_id, [])
            take = max(0, min(free_slots, len(queue)))
            tasks = queue[:take]
            del queue[:take]
            for task in tasks:
                if 'task_id' in task:
                    self._state.task_states[int(task['task_id'])] = 'running'
            return True, tasks

    def update_task_status(self, task_id: int, body: dict[str, Any]) -> bool:
        with self._lock:
            self._record('update_task_status', {'task_id': task_id, 'body': body})
            if task_id not in self._state.task_states:
                return False
            self._state.task_states[task_id] = body.get('state', self._state.task_states[task_id])
            return True

    def upload_log(self, task_id: int, body: dict[str, Any]) -> bool:
        with self._lock:
            self._record('upload_log', {'task_id': task_id, 'body': body})
            if task_id not in self._state.task_states:
                return False
            stream = body.get('stream', 'stdout')
            data = body.get('data', '')
            self._state.logs.setdefault(task_id, {})[stream] = str(data)
            return True

    def get_task_state(self, task_id: int) -> str | None:
        with self._lock:
            self._record('get_task_state', {'task_id': task_id})
            return self._state.task_states.get(task_id)

    def transactions(self) -> list[dict[str, Any]]:
        with self._lock:
            return [dict(tx) for tx in self._state.transactions]

    def get_log(self, task_id: int, stream: str) -> str:
        with self._lock:
            return self._state.logs.get(task_id, {}).get(stream, '')


class _Handler(BaseHTTPRequestHandler):
    recorder: WorkerApiRecorder

    def _read_json(self) -> dict[str, Any]:
        length = int(self.headers.get('Content-Length', '0'))
        if length <= 0:
            return {}
        body = self.rfile.read(length).decode('utf-8')
        if not body:
            return {}
        return json.loads(body)

    def log_message(self, _format: str, *_args: Any) -> None:
        return

    def do_PUT(self) -> None:  # noqa: N802
        path = urlparse(self.path).path
        if path.startswith('/api/v1/agents/'):
            agent_id = path.rsplit('/', 1)[-1]
            body = self._read_json()
            payload = self.recorder.register_agent(agent_id, body)
            _json_response(self, 200, payload)
            return
        _json_response(self, 404, {'error': {'code': 'NOT_FOUND'}})

    def do_POST(self) -> None:  # noqa: N802
        path = urlparse(self.path).path
        body = self._read_json()

        if path.startswith('/api/v1/agents/') and path.endswith('/heartbeat'):
            agent_id = path.split('/')[4]
            ok = self.recorder.send_heartbeat(agent_id, body)
            if not ok:
                _json_response(self, 404, {'error': {'code': 'AGENT_NOT_FOUND'}})
                return
            _json_response(self, 200, {'status': 'ok'})
            return

        if path.startswith('/api/v1/agents/') and path.endswith('/tasks:poll'):
            agent_id = path.split('/')[4]
            free_slots = int(body.get('free_slots', 0))
            ok, tasks = self.recorder.poll_tasks(agent_id, free_slots)
            if not ok:
                _json_response(self, 404, {'error': {'code': 'AGENT_NOT_FOUND'}})
                return
            _json_response(self, 200, {'tasks': tasks})
            return

        if path.startswith('/api/v1/tasks/') and path.endswith('/status'):
            try:
                task_id = int(path.split('/')[4])
            except (IndexError, ValueError):
                _json_response(self, 400, {'error': {'code': 'BAD_REQUEST'}})
                return
            ok = self.recorder.update_task_status(task_id, body)
            if not ok:
                _json_response(self, 404, {'error': {'code': 'TASK_NOT_FOUND'}})
                return
            _json_response(self, 200, {'status': 'ok'})
            return

        if path.startswith('/api/v1/tasks/') and path.endswith('/logs:upload'):
            try:
                task_id = int(path.split('/')[4])
            except (IndexError, ValueError):
                _json_response(self, 400, {'error': {'code': 'BAD_REQUEST'}})
                return
            ok = self.recorder.upload_log(task_id, body)
            if not ok:
                _json_response(self, 404, {'error': {'code': 'TASK_NOT_FOUND'}})
                return
            _json_response(self, 200, {'status': 'ok', 'size_bytes': len(body.get('data', ''))})
            return

        _json_response(self, 404, {'error': {'code': 'NOT_FOUND'}})

    def do_GET(self) -> None:  # noqa: N802
        path = urlparse(self.path).path
        if path == '/health':
            _json_response(self, 200, {'status': 'ok'})
            return
        if path.startswith('/api/v1/tasks/'):
            try:
                task_id = int(path.rsplit('/', 1)[-1])
            except ValueError:
                _json_response(self, 400, {'error': {'code': 'BAD_REQUEST'}})
                return
            state = self.recorder.get_task_state(task_id)
            if state is None:
                _json_response(self, 404, {'error': {'code': 'TASK_NOT_FOUND'}})
                return
            _json_response(self, 200, {'task': {'task_id': task_id, 'state': state}})
            return
        _json_response(self, 404, {'error': {'code': 'NOT_FOUND'}})


class MockMasterForWorkerServer:
    def __init__(self, host: str, port: int, recorder: WorkerApiRecorder) -> None:
        handler_cls = type('WorkerHandler', (_Handler,), {})
        handler_cls.recorder = recorder
        self._httpd = ThreadingHTTPServer((host, port), handler_cls)
        self._thread = threading.Thread(target=self._httpd.serve_forever, daemon=True)

    @property
    def host(self) -> str:
        return self._httpd.server_address[0]

    @property
    def port(self) -> int:
        return int(self._httpd.server_address[1])

    def start(self) -> None:
        self._thread.start()

    def stop(self) -> None:
        self._httpd.shutdown()
        self._httpd.server_close()
        self._thread.join(timeout=5)
