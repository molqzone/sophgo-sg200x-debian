BOARD ?= maixcam
STORAGE ?= sd
OUTPUT_DIR ?= output

.DEFAULT_GOAL := verify

.PHONY: test firmware modules image verify toolchain matrix clean-output

test:
	@bash scripts/ci/local-build.sh test --board "$(BOARD)" --storage "$(STORAGE)" --output "$(OUTPUT_DIR)"

firmware:
	@bash scripts/ci/local-build.sh firmware --board "$(BOARD)" --storage "$(STORAGE)" --output "$(OUTPUT_DIR)"

modules:
	@bash scripts/ci/local-build.sh modules --board "$(BOARD)" --storage "$(STORAGE)" --output "$(OUTPUT_DIR)"

image:
	@bash scripts/ci/local-build.sh image --board "$(BOARD)" --storage "$(STORAGE)" --output "$(OUTPUT_DIR)"

verify:
	@bash scripts/ci/local-build.sh verify --board "$(BOARD)" --storage "$(STORAGE)" --output "$(OUTPUT_DIR)"

toolchain:
	@bash scripts/ci/local-build.sh toolchain --board "$(BOARD)" --storage "$(STORAGE)" --output "$(OUTPUT_DIR)"

matrix:
	@python3 scripts/ci/plan.py matrix

clean-output:
	@find "$(OUTPUT_DIR)" -mindepth 1 -maxdepth 1 -type f -delete
