.PHONY: firmware-build firmware-upload firmware-monitor backend-install backend-venv backend-test backend-lint backend-run contract-generate contract-check

PYTHON ?= python3
PIO := PLATFORMIO_CORE_DIR=$(CURDIR)/.platformio $(CURDIR)/.venv/bin/pio
BACKEND_PYTHON := $(CURDIR)/backend/.venv/bin/python
BACKEND_PYTEST := $(CURDIR)/backend/.venv/bin/pytest
BACKEND_RUFF := $(CURDIR)/backend/.venv/bin/ruff
BACKEND_UVICORN := $(CURDIR)/backend/.venv/bin/uvicorn

firmware-build:
	$(PIO) run

firmware-upload:
	$(PIO) run --target upload

firmware-monitor:
	$(PIO) device monitor

backend-venv:
	$(PYTHON) -m venv backend/.venv
	$(BACKEND_PYTHON) -m pip install -e './backend[dev]'

backend-install:
	$(PYTHON) -m venv backend/.venv
	$(BACKEND_PYTHON) -m pip install -e ./backend

backend-test: contract-check
	$(BACKEND_PYTEST) backend/tests

backend-lint:
	$(BACKEND_RUFF) check backend
	$(BACKEND_RUFF) format --check backend

backend-run:
	PRESENCE_DB_PATH=$(CURDIR)/data/presence.db \
	$(BACKEND_UVICORN) presence_api.main:app --app-dir backend/src \
		--host 127.0.0.1 --port 8081

contract-generate:
	$(BACKEND_PYTHON) backend/scripts/export_telemetry_schema.py
	$(BACKEND_PYTHON) backend/scripts/export_openapi.py

contract-check:
	$(BACKEND_PYTHON) backend/scripts/export_telemetry_schema.py --check
	$(BACKEND_PYTHON) backend/scripts/export_openapi.py --check
