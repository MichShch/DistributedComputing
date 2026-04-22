from __future__ import annotations

import pathlib

import pytest

from tests.utils.process import combined_output


@pytest.mark.smoke
@pytest.mark.parametrize(
    ('binary_fixture', 'args', 'expected_fragment'),
    [
        ('dc_cli_bin', ['--help'], 'Usage: dc_cli'),
        ('dc_worker_bin', ['--help'], 'Usage: dc_worker'),
        ('dc_master_bin', ['--help'], 'Usage: dc_master'),
    ],
)
def test_binary_help_outputs_usage(
    request: pytest.FixtureRequest,
    run_binary,
    binary_fixture: str,
    args: list[str],
    expected_fragment: str,
) -> None:
    binary = request.getfixturevalue(binary_fixture)
    result = run_binary(binary, *args)
    output = combined_output(result.stdout, result.stderr)

    assert result.returncode == 0, output
    assert expected_fragment in output


@pytest.mark.integration
@pytest.mark.parametrize(
    'binary_fixture',
    ['dc_cli_bin', 'dc_worker_bin', 'dc_master_bin'],
)
def test_binary_unknown_option_returns_non_zero(
    request: pytest.FixtureRequest,
    run_binary,
    binary_fixture: str,
) -> None:
    binary: pathlib.Path = request.getfixturevalue(binary_fixture)
    result = run_binary(binary, '--definitely-unknown-arg')
    output = combined_output(result.stdout, result.stderr)

    assert result.returncode != 0
    assert 'Unknown option' in output
