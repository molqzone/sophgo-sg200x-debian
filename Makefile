BOARD ?= maixcam
STORAGE ?= sd
OUTPUT_DIR ?= output

.DEFAULT_GOAL := verify

.PHONY: test firmware modules middleware image verify toolchain matrix clean-output sync

test:
	@bash scripts/ci/local-build.sh test --board "$(BOARD)" --storage "$(STORAGE)" --output "$(OUTPUT_DIR)"

firmware:
	@bash scripts/ci/local-build.sh firmware --board "$(BOARD)" --storage "$(STORAGE)" --output "$(OUTPUT_DIR)"

modules:
	@bash scripts/ci/local-build.sh modules --board "$(BOARD)" --storage "$(STORAGE)" --output "$(OUTPUT_DIR)"

middleware:
	@bash scripts/ci/local-build.sh middleware --board "$(BOARD)" --storage "$(STORAGE)" --output "$(OUTPUT_DIR)"

image:
	@bash scripts/ci/local-build.sh image --board "$(BOARD)" --storage "$(STORAGE)" --output "$(OUTPUT_DIR)"

verify:
	@bash scripts/ci/local-build.sh verify --board "$(BOARD)" --storage "$(STORAGE)" --output "$(OUTPUT_DIR)"

toolchain:
	@bash scripts/ci/local-build.sh toolchain --board "$(BOARD)" --storage "$(STORAGE)" --output "$(OUTPUT_DIR)"

matrix:
	@python3 scripts/ci/plan.py matrix

# 把运行时文件推到已经启动的板子上（或挂载好的卡里），不重建镜像。见 docs/dev-sync.md。
# Push the runtime files onto an already-booted board (or a mounted card) instead of
# rebuilding the image.  See docs/dev-sync.md.
sync:
	@python3 scripts/ci/dev_sync.py $(ARGS)

# 在线时把 pin 的所有仓库做成离线镜像：以后 clean 掉了检出也能离线重建。见 scripts/ci/mirror-sources.sh。
# Populate the offline source mirrors while the network works; then a deleted checkout is
# recoverable without it.
cache-mirror:
	@bash scripts/ci/mirror-sources.sh $(ARGS)

.PHONY: cache-mirror

clean-output:
	@find "$(OUTPUT_DIR)" -mindepth 1 -maxdepth 1 -type f -delete
