$(BUILDDIR)/rtos-firmware-stamp:
	@echo "$(COLOUR_GREEN)Installing remoteproc RTOS firmware for $(BOARD)$(END_COLOUR)"
	@mkdir -p $(BUILDDIR)/rtos-firmware-sdk
	@if [ ! -d $(BUILDDIR)/rtos-firmware-sdk/.git ]; then \
		git clone --depth 1 https://github.com/milkv-duo/duo-buildroot-sdk-v2.git $(BUILDDIR)/rtos-firmware-sdk; \
	fi
	@cp -a /configs/$(BOARD)/memmap.py $(BUILDDIR)/rtos-firmware-sdk/build/boards/cv181x/sg2002_milkv_duo256m_musl_riscv64_sd/memmap.py
	@cd $(BUILDDIR)/rtos-firmware-sdk && bash -lc ' \
		export PATH=/host-tools/gcc/riscv64-elf-x86_64/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:$$PATH; \
		source build/envsetup_milkv.sh milkv-duo256m-musl-riscv64-sd >/dev/null; \
		build_rtos'
	@mkdir -p /rootfs/lib/firmware
	@cp -a $(BUILDDIR)/rtos-firmware-sdk/freertos/cvitek/install/bin/cvirtos.elf /rootfs/lib/firmware/c906-mcu.elf
	@cp -a $(BUILDDIR)/rtos-firmware-sdk/freertos/cvitek/install/bin/cvirtos.elf /output/$(BOARD)_c906-mcu.elf
	@cp -a $(BUILDDIR)/rtos-firmware-sdk/freertos/cvitek/install/bin/cvirtos.bin /output/$(BOARD)_c906-mcu.bin
	@mkdir -p /rootfs/usr/bin/
	@cp -a addons/rtos-firmware/rtos-mode /rootfs/usr/bin/
	@chmod +x /rootfs/usr/bin/rtos-mode
	@touch $@
