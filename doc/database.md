# База данных (PostgreSQL)

## 1. Назначение
База данных хранит состояние агентов и задач, историю назначений

## 2. Основные сущности
- agents: зарегистрированные агенты и их ресурсы.
- tasks: задачи и их жизненный цикл.
- task_assignments: история назначений задач агентам.

## 3. Типы (ENUM)
Рекомендуется завести типы:
- agent_status: Idle, Busy, Offline
- task_state: Queued, Running, Succeeded, Failed, Canceled

## 4. Таблицы и поля

### 4.1 agents
- agent_id (PK, text)
- os (text)
- version (text)
- resources_cpu_cores (int)
- resources_ram_mb (int)
- resources_slots (int)
- status (agent_status)
- last_heartbeat (timestamptz)

### 4.2 tasks
- task_id (PK, bigserial)
- state (task_state)
- command (text)
- args (jsonb) — массив строк
- env (jsonb) — map<string,string>
- constraints (jsonb) — объект ограничений (os/cpu_cores/ram_mb/labels)
- timeout_sec (int, nullable)
- assigned_agent (text, FK -> agents.agent_id, nullable)
- created_at (timestamptz)
- started_at (timestamptz, nullable)
- finished_at (timestamptz, nullable)
- exit_code (int, nullable)
- error_message (text, nullable)

### 4.3 task_assignments
- id (bigserial, PK)
- task_id (bigint, FK -> tasks.task_id)
- agent_id (FK -> agents.agent_id)
- assigned_at (timestamptz)
- unassigned_at (timestamptz, nullable)
- reason (text, nullable)

## 5. Индексы
Минимальный набор:
- agents(status)
- agents(last_heartbeat)
- tasks(state, created_at)
- tasks(assigned_agent)
- tasks(created_at)
- tasks(constraints) [GIN]
- task_assignments(task_id, assigned_at)
- task_assignments(agent_id, assigned_at)

## 6. Связи и целостность
- tasks.assigned_agent -> agents.agent_id (nullable)
- task_assignments.task_id -> tasks.task_id (ON DELETE CASCADE)
- task_assignments.agent_id -> agents.agent_id

## 7. Миграции PostgreSQL
Скрипт `scripts/init_pg.py` запускает `yoyo`-миграции из `migrations_broker_pg`.

### 7.1 Передача параметров
Требуется установленный Python 3 и пакет `yoyo-migrations`.
Параметры берутся из переменных окружения или из файла конфигурации (формат `.env` с `KEY=VALUE`):
- `DB_HOST` (по умолчанию `localhost`)
- `DB_PORT` (по умолчанию `5432`)
- `DB_USER` (обязательно)
- `DB_PASSWORD` (может быть пустым)
- `DB_NAME` (обязательно)
- `PG_SSLMODE` (опционально)
- `DB_CONFIG` (путь к файлу конфигурации)
- `MIGRATIONS_DIR` (опционально, путь к директории миграций)

CLI-параметры `--host/--port/--user/--password/--dbname/--sslmode` имеют приоритет над окружением и конфигом.

Пример `.env`:
```
DB_HOST=127.0.0.1
DB_PORT=5432
DB_USER=postgres
DB_PASSWORD=secret
DB_NAME=distributed
```

Запуск:
```
python3 scripts/init_pg.py --config configs/db.env
```

## 8. MongoDB backend
Master also supports MongoDB storage when `DB_BACKEND=mongo`.

Required variables for Mongo mode:
- `DB_HOST` (default: `localhost`)
- `DB_PORT` (default: `27017`)
- `DB_NAME` (database name)
- `DB_AUTHMODE` (`password` or `ssl`, default: `password`)
- `DB_USER` and `DB_PASSWORD` (required for `DB_AUTHMODE=password`)
- `DB_MONGO_AUTH_SOURCE` (optional for password auth, default: `admin`)
- `DB_SSL_ROOTCERT` / `DB_SSL_CERT` / `DB_SSL_KEY` (for `DB_AUTHMODE=ssl`)

Behavior notes:
- `scripts/init_pg.py` is used only for `DB_BACKEND=postgres`.
- In Mongo mode, startup runs `scripts/init_mongo.py` (or `INIT_DB_SCRIPT` override),
  which executes `mongodb-migrations` from `migrations_broker_mongo`.
- Collections used by Master: `agents`, `tasks`, `task_assignments`, `counters`.
