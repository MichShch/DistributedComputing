from __future__ import annotations

import pathlib
import shutil
import subprocess
from typing import Callable

import pytest

from tests.utils.process import combined_output


@pytest.mark.smoke
@pytest.mark.docker
@pytest.mark.parametrize(
    ('target', 'expected_fragment'),
    [
        ('master', 'Usage:'),
        ('worker', 'Usage:'),
        ('cli', 'Usage:'),
    ],
)
def test_docker_image_build_and_help_run(
    docker_available: None,
    docker_tag_prefix: str,
    run_command: Callable[..., subprocess.CompletedProcess[str]],
    dc_master_bin: pathlib.Path,
    dc_worker_bin: pathlib.Path,
    dc_cli_bin: pathlib.Path,
    target: str,
    expected_fragment: str,
) -> None:
    del docker_available
    repo_root = pathlib.Path(__file__).resolve().parents[1]
    bin_dir = repo_root / 'bin'
    bin_dir.mkdir(parents=True, exist_ok=True)
    shutil.copy2(dc_master_bin, bin_dir / 'dc_master')
    shutil.copy2(dc_worker_bin, bin_dir / 'dc_worker')
    shutil.copy2(dc_cli_bin, bin_dir / 'dc_cli')

    image_tag = f'{docker_tag_prefix}-{target}:latest'

    build = run_command(
        'docker',
        'build',
        '--file',
        'Dockerfile',
        '--target',
        target,
        '--tag',
        image_tag,
        '.',
        timeout=600,
        cwd=repo_root,
    )
    build_output = combined_output(build.stdout, build.stderr)
    assert build.returncode == 0, (
        f'docker build failed for target={target}, tag={image_tag}\n'
        f'{build_output}'
    )

    run = run_command(
        'docker',
        'run',
        '--rm',
        image_tag,
        '--help',
        timeout=30,
        cwd=repo_root,
    )
    run_output = combined_output(run.stdout, run.stderr)
    assert run.returncode == 0, (
        f'docker run failed for target={target}, tag={image_tag}\n'
        f'{run_output}'
    )
    assert expected_fragment in run_output, run_output
