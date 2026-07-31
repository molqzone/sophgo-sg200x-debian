RTOS_FIRMWARE_SDK := $(BUILDDIR)/rtos-firmware-sdk
RTOS_FIRMWARE_SDK_REPO := https://github.com/milkv-duo/duo-buildroot-sdk-v2.git
RTOS_FIRMWARE_SDK_COMMIT := 6f8962c394dd0a05729abb089f0feb7d5cc4aa5e
RTOS_FIRMWARE_ELF := $(RTOS_FIRMWARE_SDK)/freertos/cvitek/install/bin/cvirtos.elf
RTOS_FIRMWARE_BIN := $(RTOS_FIRMWARE_SDK)/freertos/cvitek/install/bin/cvirtos.bin
RTOS_LINUX_TOOLS := $(BUILDDIR)/rtos-linux-tools

$(BUILDDIR)/rtos-firmware-build-stamp:
	@echo "$(COLOUR_GREEN)Building RTOS firmware for $(BOARD)$(END_COLOUR)"
	@mkdir -p $(RTOS_FIRMWARE_SDK)
	@if [ ! -d $(RTOS_FIRMWARE_SDK)/.git ]; then \
		git -C $(RTOS_FIRMWARE_SDK) init; \
	fi
	@if ! git -C $(RTOS_FIRMWARE_SDK) remote get-url origin >/dev/null 2>&1; then \
		git -C $(RTOS_FIRMWARE_SDK) remote add origin $(RTOS_FIRMWARE_SDK_REPO); \
	fi
	@git -C $(RTOS_FIRMWARE_SDK) remote set-url origin $(RTOS_FIRMWARE_SDK_REPO)
	@git -C $(RTOS_FIRMWARE_SDK) fetch --depth 1 origin $(RTOS_FIRMWARE_SDK_COMMIT)
	@git -C $(RTOS_FIRMWARE_SDK) checkout --force --detach FETCH_HEAD
	@test "$$(git -C $(RTOS_FIRMWARE_SDK) rev-parse HEAD)" = "$(RTOS_FIRMWARE_SDK_COMMIT)"
	@cp -a /configs/$(BOARD)/memmap.py $(RTOS_FIRMWARE_SDK)/build/boards/cv181x/sg2002_milkv_duo256m_musl_riscv64_sd/memmap.py
	@bash /builder/addons/rtos-firmware/prepare_sdk.sh $(RTOS_FIRMWARE_SDK)
	@cd $(RTOS_FIRMWARE_SDK) && bash -lc ' \
		export PATH=/host-tools/gcc/riscv64-elf-x86_64/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:$$PATH; \
		source build/envsetup_milkv.sh milkv-duo256m-musl-riscv64-sd >/dev/null; \
		build_rtos'
	@test -s $(RTOS_FIRMWARE_ELF)
	@test -s $(RTOS_FIRMWARE_BIN)
	@python3 /builder/check_rtos_trace_layout.py \
		--readelf /host-tools/gcc/riscv64-elf-x86_64/bin/riscv64-unknown-elf-readelf \
		$(RTOS_FIRMWARE_ELF) | tee /output/$(BOARD)_boot-trace-layout.json
	@sha256sum $(RTOS_FIRMWARE_ELF) $(RTOS_FIRMWARE_BIN)
	@touch $@

$(BUILDDIR)/rtos-firmware-stamp: $(BUILDDIR)/rtos-firmware-build-stamp
	@echo "$(COLOUR_GREEN)Installing RTOS firmware for $(BOARD)$(END_COLOUR)"
	@mkdir -p /rootfs/lib/firmware
	@cp -a $(RTOS_FIRMWARE_ELF) /rootfs/lib/firmware/c906-mcu.elf
	@cp -a $(RTOS_FIRMWARE_ELF) /output/$(BOARD)_c906-mcu.elf
	@cp -a $(RTOS_FIRMWARE_BIN) /output/$(BOARD)_c906-mcu.bin
	@git -C $(RTOS_FIRMWARE_SDK) rev-parse HEAD > /output/$(BOARD)_rtos-sdk-commit.txt
	@CC=/host-tools/gcc/riscv64-linux-musl-x86_64/bin/riscv64-unknown-linux-musl-gcc \
		AR=/host-tools/gcc/riscv64-linux-musl-x86_64/bin/riscv64-unknown-linux-musl-ar \
		bash /builder/addons/rtos-firmware/build_linux_tools.sh \
		$(RTOS_FIRMWARE_SDK)/cvi_mpi/include $(RTOS_LINUX_TOOLS)
	@mkdir -p /rootfs/usr/bin /rootfs/usr/include /rootfs/usr/lib
	@cp -a $(RTOS_LINUX_TOOLS)/rtos-cmd /rootfs/usr/bin/rtos-cmd
	@cp -a $(RTOS_LINUX_TOOLS)/rtos-bench /rootfs/usr/bin/rtos-bench
	@cp -a $(RTOS_LINUX_TOOLS)/libsg2002-rtos.a /rootfs/usr/lib/
	@cp -a /builder/addons/rtos-firmware/linux/include/sg2002_rtos.h /rootfs/usr/include/
	@cp -a /builder/addons/rtos-firmware/include/sg2002_rtos_protocol.h /rootfs/usr/include/
	@cp -a /builder/addons/rtos-firmware/include/sg2002_rtos_shm.h /rootfs/usr/include/
	@cp -a addons/rtos-firmware/rtos-mode /rootfs/usr/bin/
	@chmod +x /rootfs/usr/bin/rtos-cmd
	@chmod +x /rootfs/usr/bin/rtos-bench
	@chmod +x /rootfs/usr/bin/rtos-mode
	@cp -a $(RTOS_LINUX_TOOLS)/rtos-cmd /output/$(BOARD)_rtos-cmd
	@cp -a $(RTOS_LINUX_TOOLS)/rtos-bench /output/$(BOARD)_rtos-bench
	@cp -a $(RTOS_LINUX_TOOLS)/libsg2002-rtos.a /output/$(BOARD)_libsg2002-rtos.a
	@cp -a /builder/addons/rtos-firmware/linux/include/sg2002_rtos.h /output/
	@cp -a /builder/addons/rtos-firmware/include/sg2002_rtos_protocol.h /output/
	@cp -a /builder/addons/rtos-firmware/include/sg2002_rtos_shm.h /output/
	@touch $@
