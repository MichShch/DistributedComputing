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
- task_id (PK, text)
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
- task_id (FK -> tasks.task_id)
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

## 7. Скрипт инициализации
Скрипт `scripts/init_db.py` создаёт БД и схему (если БД отсутствует) либо проверяет соответствие схемы
описанию и выводит различия.

### 7.1 Передача параметров
Требуется установленный Python 3 и пакет `psycopg2-binary`.
Параметры берутся из переменных окружения или из файла конфигурации (формат `.env` с `KEY=VALUE`):
- `DB_HOST` (по умолчанию `localhost`)
- `DB_PORT` (по умолчанию `5432`)
- `DB_USER` (обязательно)
- `DB_PASSWORD` (может быть пустым)
- `DB_NAME` (обязательно)
- `DB_SSLMODE` (опционально)
- `DB_CONFIG` (путь к файлу конфигурации)

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
python3 scripts/init_db.py --config configs/db.env
```
