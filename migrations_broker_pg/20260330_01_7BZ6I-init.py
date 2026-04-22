'''
Init postgresql db
'''

from yoyo import step

__depends__ = {}

steps = [
    step('''
    CREATE TYPE agent_status AS ENUM ('Idle', 'Busy', 'Offline');
    CREATE TYPE task_state AS ENUM ('Queued', 'Running', 'Succeeded', 'Failed', 'Canceled');
    CREATE TABLE agents (
        agent_id TEXT PRIMARY KEY,
        os TEXT NOT NULL,
        version TEXT NOT NULL,
        resources_cpu_cores INT NOT NULL,
        resources_ram_mb INT NOT NULL,
        resources_slots INT NOT NULL,
        status agent_status NOT NULL,
        last_heartbeat TIMESTAMPTZ NOT NULL
    );
    CREATE TABLE tasks (
        task_id BIGSERIAL PRIMARY KEY,
        state task_state NOT NULL,
        command TEXT NOT NULL,
        args JSONB NOT NULL,
        env JSONB NOT NULL,
        constraints JSONB NOT NULL DEFAULT '{}'::jsonb,
        timeout_sec INT,
        assigned_agent TEXT,
        created_at TIMESTAMPTZ NOT NULL,
        started_at TIMESTAMPTZ,
        finished_at TIMESTAMPTZ,
        exit_code INT,
        error_message TEXT,
        CONSTRAINT tasks_assigned_agent_fk FOREIGN KEY (assigned_agent)
            REFERENCES agents(agent_id)
    );
    CREATE TABLE task_assignments (
        id BIGSERIAL PRIMARY KEY,
        task_id BIGINT NOT NULL,
        agent_id TEXT NOT NULL,
        assigned_at TIMESTAMPTZ NOT NULL,
        unassigned_at TIMESTAMPTZ,
        reason TEXT,
        CONSTRAINT task_assignments_task_id_fk FOREIGN KEY (task_id)
            REFERENCES tasks(task_id) ON DELETE CASCADE,
        CONSTRAINT task_assignments_agent_id_fk FOREIGN KEY (agent_id)
            REFERENCES agents(agent_id) ON DELETE CASCADE
    );
    CREATE INDEX idx_agents_status ON agents(status);
    CREATE INDEX idx_agents_last_heartbeat ON agents(last_heartbeat);
    CREATE INDEX idx_tasks_state_created_at ON tasks(state, created_at);
    CREATE INDEX idx_tasks_assigned_agent ON tasks(assigned_agent);
    CREATE INDEX idx_tasks_created_at ON tasks(created_at);
    CREATE INDEX idx_tasks_constraints ON tasks USING GIN (constraints);
    CREATE INDEX idx_task_assignments_task_id_assigned_at ON task_assignments(task_id, assigned_at);
    CREATE INDEX idx_task_assignments_agent_id_assigned_at ON task_assignments(agent_id, assigned_at);
    ''')
]
