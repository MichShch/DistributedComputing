FROM ubuntu:24.04 AS worker

ENV MAIN=/home/main
ENV MASTER_URL=http://localhost:8080
ENV AGENT_ID=worker-1
ENV AGENT_OS=linux
ENV AGENT_VERSION=dev
ENV CPU_CORES=1
ENV RAM_MB=0
ENV SLOTS=1
ENV WORKER_HTTP_TIMEOUT_MS=5000
ENV WORKER_LOG_DIR=logs/worker
ENV CANCEL_CHECK_SEC=1
ENV UPLOAD_LOGS=true
ENV MAX_UPLOAD_BYTES=10485760
ENV WORKER_LOG_LEVEL=info
ENV WORKER_LOG_FILE=logs/worker/worker.log
WORKDIR $MAIN

COPY bin/dc_worker ./worker
RUN chmod +x ./worker

ENTRYPOINT ["./worker"]

FROM ubuntu:24.04 AS master

ENV DEBIAN_FRONTEND=noninteractive
ENV MAIN=/home/main
ENV VIRTUAL_ENV=/opt/venv
ENV PATH="${VIRTUAL_ENV}/bin:${PATH}"
ENV DB_HOST=localhost
ENV DB_PORT=5432
ENV DB_NAME=distributed
ENV DB_BACKEND=postgres
ENV DB_AUTHMODE=password
ENV DB_USER=postgres
ENV DB_PASSWORD=
ENV DB_SSL_ROOTCERT=
ENV DB_SSL_CERT=
ENV DB_SSL_KEY=
ENV MASTER_HOST=0.0.0.0
ENV MASTER_PORT=8080
ENV LOG_DIR=logs
ENV MASTER_LOG_FILE=logs/master.log
ENV MASTER_LOG_LEVEL=trace
ENV HEARTBEAT_SEC=30
ENV OFFLINE_SEC=120
ENV BROKER_RECONNECT_ATTEMPTS=5
ENV BROKER_RECONNECT_COOLDOWN_SEC=2
ENV MAX_LOG_UPLOAD_BYTES=10485760
ENV INIT_DB_PYTHON=python3
ENV INIT_DB_SCRIPT=/home/main/init_pg.py
ENV MIGRATIONS_DIR=/home/main/migrations_broker_pg
ENV MONGO_MIGRATIONS_METASTORE=database_migrations
ENV MASTER_SKIP_DB_MIGRATION=false

WORKDIR $MAIN

RUN apt-get update \
    && apt-get install -y --no-install-recommends libpq5 libsnappy1v5 \
    && rm -rf /var/lib/apt/lists/*

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        python3 \
        python3-pip \
        python3-venv \
        python3-dev \
        build-essential \
        libpq-dev \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

COPY src/master/requirements.txt requirements.txt
RUN python3 -m venv "${VIRTUAL_ENV}" \
    && pip install --no-cache-dir -r requirements.txt

COPY bin/dc_master ./master
RUN chmod +x ./master

COPY scripts/init_pg.py .
COPY scripts/init_mongo.py .

COPY migrations_broker_pg migrations_broker_pg
COPY migrations_broker_mongo migrations_broker_mongo

ENTRYPOINT ["./master"]

FROM ubuntu:24.04 AS cli

ENV MAIN=/home/main
ENV MASTER_HOST=127.0.0.1
ENV MASTER_PORT=8080
WORKDIR $MAIN

COPY bin/dc_cli ./cli
RUN chmod +x ./cli

ENTRYPOINT ["./cli"]
