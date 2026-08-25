.PHONY: firmware-build firmware-unit firmware-upload firmware-monitor tools-test backend-install backend-venv backend-test backend-lint backend-run contract-generate contract-check

PYTHON ?= python3
PIO := PLATFORMIO_CORE_DIR=$(CURDIR)/.platformio $(CURDIR)/.venv/bin/pio
BACKEND_PYTHON := $(CURDIR)/backend/.venv/bin/python
BACKEND_PYTEST := $(CURDIR)/backend/.venv/bin/pytest
BACKEND_RUFF := $(CURDIR)/backend/.venv/bin/ruff
BACKEND_UVICORN := $(CURDIR)/backend/.venv/bin/uvicorn
NATIVE_BUILD := $(CURDIR)/.build/native

firmware-build:
	$(PIO) run

firmware-unit:
	mkdir -p $(NATIVE_BUILD)
	$(CXX) -std=c++17 -Wall -Wextra -Werror -Isrc \
		test/native/test_telemetry_queue.cpp src/telemetry.cpp \
		-o $(NATIVE_BUILD)/telemetry_queue_test
	$(NATIVE_BUILD)/telemetry_queue_test
	$(CXX) -std=c++17 -Wall -Wextra -Werror -Isrc \
		test/native/test_telemetry_json.cpp src/telemetry_json.cpp \
		-o $(NATIVE_BUILD)/telemetry_json_test
	$(NATIVE_BUILD)/telemetry_json_test
	$(CXX) -std=c++17 -Wall -Wextra -Werror -Isrc \
		test/native/test_ingest_ack.cpp src/ingest_ack.cpp \
		-o $(NATIVE_BUILD)/ingest_ack_test
	$(NATIVE_BUILD)/ingest_ack_test
	$(CXX) -std=c++17 -Wall -Wextra -Werror -Isrc \
		test/native/test_spool_name.cpp src/spool_name.cpp \
		-o $(NATIVE_BUILD)/spool_name_test
	$(NATIVE_BUILD)/spool_name_test
	$(CXX) -std=c++17 -Wall -Wextra -Werror -Isrc \
		test/native/test_provisioning_protocol.cpp \
		src/provisioning_protocol.cpp \
		-o $(NATIVE_BUILD)/provisioning_protocol_test
	$(NATIVE_BUILD)/provisioning_protocol_test
	$(CXX) -std=c++17 -Wall -Wextra -Werror -Isrc \
		test/native/test_device_config.cpp src/device_config.cpp \
		-o $(NATIVE_BUILD)/device_config_test
	$(NATIVE_BUILD)/device_config_test
	$(CXX) -std=c++17 -Wall -Wextra -Werror -Isrc \
		test/native/test_device_config_storage.cpp \
		src/device_config_storage.cpp src/device_config.cpp \
		-o $(NATIVE_BUILD)/device_config_storage_test
	$(NATIVE_BUILD)/device_config_storage_test
	$(CXX) -std=c++17 -Wall -Wextra -Werror -Isrc \
		test/native/test_device_config_mailbox.cpp \
		src/device_config_mailbox.cpp src/device_config.cpp \
		-o $(NATIVE_BUILD)/device_config_mailbox_test
	$(NATIVE_BUILD)/device_config_mailbox_test
	$(CXX) -std=c++17 -Wall -Wextra -Werror -Isrc \
		test/native/test_feedback_protocol.cpp src/feedback_protocol.cpp \
		-o $(NATIVE_BUILD)/feedback_protocol_test
	$(NATIVE_BUILD)/feedback_protocol_test
	$(CXX) -std=c++17 -Wall -Wextra -Werror -Isrc \
		test/native/test_feedback_spool_name.cpp \
		src/feedback_spool_name.cpp src/feedback_protocol.cpp \
		-o $(NATIVE_BUILD)/feedback_spool_name_test
	$(NATIVE_BUILD)/feedback_spool_name_test
	$(CXX) -std=c++17 -Wall -Wextra -Werror -Isrc \
		test/native/test_touch_feedback_queue.cpp \
		src/touch_feedback_queue.cpp src/feedback_protocol.cpp \
		-o $(NATIVE_BUILD)/touch_feedback_queue_test
	$(NATIVE_BUILD)/touch_feedback_queue_test
	$(CXX) -std=c++17 -Wall -Wextra -Werror -Isrc \
		test/native/test_feedback_bundle.cpp src/feedback_bundle.cpp \
		-o $(NATIVE_BUILD)/feedback_bundle_test
	$(NATIVE_BUILD)/feedback_bundle_test
	$(CXX) -std=c++17 -Wall -Wextra -Werror -Isrc \
		test/native/test_dashboard_data.cpp src/dashboard_data.cpp \
		-o $(NATIVE_BUILD)/dashboard_data_test
	$(NATIVE_BUILD)/dashboard_data_test
	$(CXX) -std=c++17 -Wall -Wextra -Werror -pthread -Isrc \
		test/native/test_dashboard_mailbox.cpp src/dashboard_mailbox.cpp \
		-o $(NATIVE_BUILD)/dashboard_mailbox_test
	$(NATIVE_BUILD)/dashboard_mailbox_test

tools-test:
	$(PYTHON) -m unittest discover -s tools/tests -v

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
