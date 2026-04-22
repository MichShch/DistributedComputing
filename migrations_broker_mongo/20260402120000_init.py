'''
Initial MongoDB schema/index migration for broker collections.
'''

from pymongo import ASCENDING, DESCENDING
from mongodb_migrations.base import BaseMigration


class Migration(BaseMigration):
    def upgrade(self) -> None:
        existing = set(self.db.list_collection_names())
        for name in ('agents', 'tasks', 'task_assignments', 'counters'):
            if name not in existing:
                self.db.create_collection(name)

        self.db.agents.create_index(
            [('agent_id', ASCENDING)],
            unique=True,
            name='agent_id_1',
        )
        self.db.agents.create_index([('status', ASCENDING)], name='status_1')
        self.db.agents.create_index(
            [('last_heartbeat', ASCENDING)],
            name='last_heartbeat_1',
        )

        self.db.tasks.create_index(
            [('task_id', ASCENDING)],
            unique=True,
            name='task_id_1',
        )
        self.db.tasks.create_index(
            [('state', ASCENDING), ('created_at', DESCENDING)],
            name='state_1_created_at_-1',
        )
        self.db.tasks.create_index(
            [('assigned_agent', ASCENDING)],
            name='assigned_agent_1',
        )
        self.db.tasks.create_index(
            [('constraints_os', ASCENDING)],
            name='constraints_os_1',
        )
        self.db.tasks.create_index(
            [('constraints_cpu_cores', ASCENDING)],
            name='constraints_cpu_cores_1',
        )
        self.db.tasks.create_index(
            [('constraints_ram_mb', ASCENDING)],
            name='constraints_ram_mb_1',
        )

        self.db.task_assignments.create_index(
            [('task_id', ASCENDING), ('assigned_at', DESCENDING)],
            name='task_id_1_assigned_at_-1',
        )
        self.db.task_assignments.create_index(
            [('agent_id', ASCENDING), ('assigned_at', DESCENDING)],
            name='agent_id_1_assigned_at_-1',
        )

    def downgrade(self) -> None:
        raise RuntimeError('Downgrade is not supported for this migration.')
