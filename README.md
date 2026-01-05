# Distributed Computing System

C++ distributed computing system with Master, Worker, and CLI components.

## Structure
- doc/ - requirements and design docs
- src/master/ - control service
- src/worker/ - worker agent
- src/cli/ - CLI client
- src/common/ - shared libraries
- api/ - OpenAPI/Proto definitions
- configs/ - example configs and policies
- scripts/ - deployment and tooling scripts
- tests/ - unit/integration/e2e tests

## Dependencies
- CMake 3.20+ and a C++17 compiler
- PostgreSQL server
- libpqxx (system dependency, required for Master)
- Python 3 with `psycopg2-binary` for `scripts/init_db.py`

### libpqxx install
- Linux (Debian/Ubuntu):
  - `sudo apt-get install libpqxx-dev libpq-dev`
- Windows (vcpkg):
  - `vcpkg install libpqxx:x64-windows`
  - then pass toolchain file to CMake: `-DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake`

## Header-only dependencies (third_party)
Place the header-only libraries into `third_party/`:
- cpp-httplib: `third_party/httplib.h`
- nlohmann/json: `third_party/nlohmann/json.hpp`

## Configuration
Database (used by Master and `scripts/init_db.py`):
- `DB_HOST` (default: `localhost`)
- `DB_PORT` (default: `5432`)
- `DB_USER` (required)
- `DB_PASSWORD` (optional)
- `DB_NAME` (required)
- `DB_SSLMODE` (optional)
- `DB_CONFIG` (optional path to `.env` file; e.g. `configs/db.env`)

Master service:
- `MASTER_HOST` (default: `0.0.0.0`)
- `MASTER_PORT` (default: `8080`)
- `LOG_DIR` (default: `logs`)
- `HEARTBEAT_SEC` (default: `30`)
- `OFFLINE_SEC` (default: `120`)
- `INIT_DB_PYTHON` (optional python executable override for init_db)

## Build
```
cmake -S . -B build
cmake --build build
```

## Tests
Integration tests for Master run via CTest and start a temporary Master instance.
They require a reachable PostgreSQL database and Python with `psycopg2-binary`.

Required env vars for tests:
- `DC_TEST_DB_USER`
- `DC_TEST_DB_NAME`

Optional overrides:
- `DC_TEST_DB_HOST` (default: `localhost`)
- `DC_TEST_DB_PORT` (default: `5432`)
- `DC_TEST_DB_PASSWORD` (default: empty)
- `DC_TEST_DB_SSLMODE` (default: empty)

Run:
```
ctest --test-dir build
```

## Run Master
```
export DB_CONFIG=configs/db.env
./build/src/master/dc_master
```

On startup the Master runs `scripts/init_db.py`. If schema differences are detected, the script prints
diffs and Master exits with code `4`.

## Example env for Master
Create a `.env` file (or reuse `configs/master.env`) and export it before запуском:
```env
DB_HOST=127.0.0.1
DB_PORT=5432
DB_USER=postgres
DB_PASSWORD=secret
DB_NAME=distributed
DB_SSLMODE=disable

MASTER_HOST=0.0.0.0
MASTER_PORT=8080
LOG_DIR=logs
HEARTBEAT_SEC=30
OFFLINE_SEC=120
```

Run with:
```
set -a
source configs/master.env
set +a
./build/src/master/dc_master
```
