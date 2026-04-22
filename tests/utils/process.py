from __future__ import annotations

import os
import pathlib
import subprocess
import time
from dataclasses import dataclass
from typing import Iterable

import requests


def combined_output(stdout: str, stderr: str) -> str:
    return f'{stdout}\n{stderr}'.strip()


@dataclass
class ManagedProcess:
    process: subprocess.Popen[str]
    args: list[str]

    def is_running(self) -> bool:
        return self.process.poll() is None


def start_process(
    args: Iterable[str],
    env: dict[str, str] | None = None,
    cwd: pathlib.Path | None = None,
) -> ManagedProcess:
    full_env = os.environ.copy()
    if env:
        full_env.update(env)
    cmd = [str(arg) for arg in args]
    proc = subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env=full_env,
        cwd=str(cwd) if cwd else None,
    )
    return ManagedProcess(process=proc, args=cmd)


def stop_process(managed: ManagedProcess, timeout: float = 5.0) -> tuple[int, str, str]:
    proc = managed.process
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            proc.kill()
    stdout, stderr = proc.communicate(timeout=timeout)
    return proc.returncode or 0, stdout, stderr


def wait_for_http_ready(
    url: str,
    managed: ManagedProcess,
    timeout_sec: float = 10.0,
    interval_sec: float = 0.1,
    acceptable_statuses: set[int] | None = None,
) -> int:
    statuses = acceptable_statuses or {200}
    deadline = time.time() + timeout_sec
    while time.time() < deadline:
        if not managed.is_running():
            break
        try:
            response = requests.get(url, timeout=1)
            if response.status_code in statuses:
                return response.status_code
        except requests.RequestException:
            pass
        time.sleep(interval_sec)
    return -1
