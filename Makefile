IDF_PATH    ?= $(HOME)/esp-idf/6.0
IDF_EXPORTS  = $(IDF_PATH)/export.sh
TARGET      ?= esp32p4

# Serial port for flash / monitor / erase-flash. Leave unset to let idf.py
# auto-detect; pass on the command line (`make flash PORT=/dev/ttyACM0`) or
# pin in Makefile.local.
PORT        ?=
IDFPY_PORT   = $(if $(PORT),-p $(PORT))

# Allow developers to keep local overrides in an untracked file.
-include Makefile.local

# Build sdkconfig defaults chains.
#   - sdkconfig.defaults                  (committed, common)
#   - sdkconfig.defaults.<target>         (committed, per-target — picked up
#     automatically by IDF, but listed explicitly here so we can chain
#     sdkconfig.defaults.local after it)
#   - sdkconfig.defaults.local            (gitignored, host-specific overrides
#     such as Wi-Fi SSID / PSK; the last entry wins for duplicate keys so
#     local values trump the committed defaults)
SDKCONFIG_DEFAULTS_HW   = sdkconfig.defaults
ifneq (,$(wildcard sdkconfig.defaults.$(TARGET)))
SDKCONFIG_DEFAULTS_HW  := $(SDKCONFIG_DEFAULTS_HW);sdkconfig.defaults.$(TARGET)
endif
ifneq (,$(wildcard sdkconfig.defaults.local))
SDKCONFIG_DEFAULTS_HW  := $(SDKCONFIG_DEFAULTS_HW);sdkconfig.defaults.local
endif

SDKCONFIG_DEFAULTS_QEMU = sdkconfig.defaults;sdkconfig.qemu
ifneq (,$(wildcard sdkconfig.defaults.local))
SDKCONFIG_DEFAULTS_QEMU := $(SDKCONFIG_DEFAULTS_QEMU);sdkconfig.defaults.local
endif

.PHONY: build build-qemu qemu qemu-kill qemu-bg clean \
        build-host test-host clean-host \
        set-target \
        build-docker docker-shell docker-clean \
        flash flash-monitor monitor monitor-log erase-flash \
        size size-components size-files sections bss-top

# Non-interactive log capture window (seconds). Used by monitor-log
# from CI / agent contexts where idf.py monitor refuses to attach.
MONITOR_LOG_SECONDS ?= 30

# Track which defaults set generated the current sdkconfig, so we
# automatically regenerate it when switching between HW and QEMU builds.
BUILD_MODE_FILE = build/.build_mode

define switch-mode
	@mkdir -p build
	@if [ "$$(cat $(BUILD_MODE_FILE) 2>/dev/null)" != "$(1)" ]; then \
		echo "switch-mode: $$(cat $(BUILD_MODE_FILE) 2>/dev/null || echo none) -> $(1); regenerating sdkconfig"; \
		rm -f sdkconfig; \
		echo "$(1)" > $(BUILD_MODE_FILE); \
	fi
endef

# --- Target builds ----------------------------------------------------------

set-target:
	bash -c "source $(IDF_EXPORTS) && idf.py -DSDKCONFIG_DEFAULTS='$(SDKCONFIG_DEFAULTS_HW)' set-target $(TARGET)"

build:
	$(call switch-mode,hw)
	bash -c "source $(IDF_EXPORTS) && idf.py -DSDKCONFIG_DEFAULTS='$(SDKCONFIG_DEFAULTS_HW)' build"

build-qemu:
	$(call switch-mode,qemu)
	bash -c "source $(IDF_EXPORTS) && idf.py -DSDKCONFIG_DEFAULTS='$(SDKCONFIG_DEFAULTS_QEMU)' build"

# Foreground QEMU. Note: ESP-IDF 6.0 ESP32-P4 QEMU support is preliminary;
# expect boot-only smoke testing, not LCD output.
qemu: build-qemu
	bash -c "source $(IDF_EXPORTS) && idf.py -DSDKCONFIG_DEFAULTS='$(SDKCONFIG_DEFAULTS_QEMU)' qemu"

qemu-kill:
	-pkill -f qemu-system 2>/dev/null || true
	-pkill -f "idf.py.*qemu$$" 2>/dev/null || true
	-pkill -f "make qemu$$" 2>/dev/null || true
	@sleep 1
	@pgrep -af "qemu-system" >/dev/null && echo "WARNING: some QEMU processes still running" || echo "QEMU stopped."

flash:
	bash -c "source $(IDF_EXPORTS) && idf.py $(IDFPY_PORT) flash"

monitor:
	bash -c "source $(IDF_EXPORTS) && idf.py $(IDFPY_PORT) monitor"

# Non-interactive log capture: resets the target via DTR/RTS and reads
# stdout for MONITOR_LOG_SECONDS seconds. Used by agent harness when
# idf.py monitor's TTY requirement gets in the way.
monitor-log:
	python3 tools/monitor_log.py --port $(if $(PORT),$(PORT),/dev/ttyACM0) --seconds $(MONITOR_LOG_SECONDS)

# Convenience: flash + monitor in a single idf.py invocation (faster than
# `make flash monitor` because the IDF env is sourced once).
flash-monitor:
	bash -c "source $(IDF_EXPORTS) && idf.py $(IDFPY_PORT) flash monitor"

erase-flash:
	bash -c "source $(IDF_EXPORTS) && idf.py $(IDFPY_PORT) erase-flash"

# Memory usage reports. `size` prints the overall .text/.bss/.rodata
# breakdown; `size-components` and `size-files` drill into where the
# bytes come from.
size:
	bash -c "source $(IDF_EXPORTS) && idf.py size"

size-components:
	bash -c "source $(IDF_EXPORTS) && idf.py size-components"

size-files:
	bash -c "source $(IDF_EXPORTS) && idf.py size-files"

# Print ELF section headers — useful to inspect where .bss / .data /
# .dram0 actually live in the address map.
sections:
	bash -c "source $(IDF_EXPORTS) && riscv32-esp-elf-objdump -h build/tab5_claude_client.elf"

# List the largest .bss symbols by size — useful when chasing down
# which component is bloating internal SRAM.
bss-top:
	bash -c "source $(IDF_EXPORTS) && riscv32-esp-elf-nm --size-sort --print-size build/tab5_claude_client.elf 2>/dev/null | grep ' [bB] ' | tail -30"

clean:
	bash -c "source $(IDF_EXPORTS) && idf.py fullclean"

# --- Host build / test ------------------------------------------------------

build-host:
	cmake -S host_test -B build-host
	cmake --build build-host -j

test-host: build-host
	ctest --test-dir build-host --output-on-failure

clean-host:
	rm -rf build-host

# --- Docker build (mirrors CI) ----------------------------------------------
DOCKER_IMAGE         ?= espressif/idf:release-v6.0
DOCKER_WORKDIR       ?= /work
DOCKER_BUILD_DIR     ?= build-docker
DOCKER_SDKCONFIG     ?= sdkconfig.docker
DOCKER_TARGET_CHIP   ?= esp32p4

define docker-run
	docker run --rm -t \
		-u $$(id -u):$$(id -g) \
		-v "$(CURDIR):$(DOCKER_WORKDIR)" \
		-w $(DOCKER_WORKDIR) \
		-e HOME=/tmp \
		-e CI=true \
		$(DOCKER_IMAGE) \
		bash -c '\
			git config --global --add safe.directory "$(DOCKER_WORKDIR)" && \
			. "$$IDF_PATH/export.sh" && \
			$(1)'
endef

build-docker:
	$(call docker-run, \
		idf.py -B $(DOCKER_BUILD_DIR) -DSDKCONFIG=$(DOCKER_SDKCONFIG) \
		       -DSDKCONFIG_DEFAULTS=sdkconfig.defaults \
		       set-target $(DOCKER_TARGET_CHIP) && \
		idf.py -B $(DOCKER_BUILD_DIR) -DSDKCONFIG=$(DOCKER_SDKCONFIG) \
		       -DSDKCONFIG_DEFAULTS=sdkconfig.defaults build)

docker-clean:
	$(call docker-run, \
		idf.py -B $(DOCKER_BUILD_DIR) -DSDKCONFIG=$(DOCKER_SDKCONFIG) \
		       fullclean)
	@rm -f $(DOCKER_SDKCONFIG)

docker-shell:
	docker run --rm -it \
		-u $$(id -u):$$(id -g) \
		-v "$(CURDIR):$(DOCKER_WORKDIR)" \
		-w $(DOCKER_WORKDIR) \
		-e HOME=/tmp \
		$(DOCKER_IMAGE) \
		bash -c '\
			git config --global --add safe.directory "$(DOCKER_WORKDIR)" && \
			. "$$IDF_PATH/export.sh" && \
			exec bash'
