from __future__ import annotations

import importlib.util
import pathlib


def _load_init_mongo_module():
    repo_root = pathlib.Path(__file__).resolve().parents[1]
    script_path = repo_root / 'scripts' / 'init_mongo.py'
    spec = importlib.util.spec_from_file_location('init_mongo_script', script_path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_build_mongo_url_uses_default_auth_source_admin() -> None:
    module = _load_init_mongo_module()
    url, temp_files = module.build_mongo_url(
        host='127.0.0.1',
        port='27017',
        user='u',
        password='p',
        authmode='password',
    )
    assert 'authSource=admin' in url
    assert temp_files == []


def test_build_mongo_url_uses_custom_auth_source() -> None:
    module = _load_init_mongo_module()
    url, _ = module.build_mongo_url(
        host='127.0.0.1',
        port='27017',
        user='u',
        password='p',
        authmode='password',
        auth_source='dc_test',
    )
    assert 'authSource=dc_test' in url
