RTOS_FIRMWARE_SDK := $(BUILDDIR)/rtos-firmware-sdk
RTOS_FIRMWARE_ELF := $(RTOS_FIRMWARE_SDK)/freertos/cvitek/install/bin/cvirtos.elf
RTOS_FIRMWARE_BIN := $(RTOS_FIRMWARE_SDK)/freertos/cvitek/install/bin/cvirtos.bin

$(BUILDDIR)/rtos-firmware-build-stamp:
	@echo "$(COLOUR_GREEN)Building RTOS firmware for $(BOARD)$(END_COLOUR)"
	@mkdir -p $(RTOS_FIRMWARE_SDK)
	@if [ ! -d $(RTOS_FIRMWARE_SDK)/.git ]; then \
		git clone --depth 1 https://github.com/milkv-duo/duo-buildroot-sdk-v2.git $(RTOS_FIRMWARE_SDK); \
	fi
	@cp -a /configs/$(BOARD)/memmap.py $(RTOS_FIRMWARE_SDK)/build/boards/cv181x/sg2002_milkv_duo256m_musl_riscv64_sd/memmap.py
	@cp -a /builder/addons/rtos-firmware/patches/freertos/cvitek/task/comm/src/riscv64/comm_main.c $(RTOS_FIRMWARE_SDK)/freertos/cvitek/task/comm/src/riscv64/comm_main.c
	@cp -a /builder/addons/rtos-firmware/patches/freertos/cvitek/task/comm/CMakeLists.txt $(RTOS_FIRMWARE_SDK)/freertos/cvitek/task/comm/CMakeLists.txt
	@cp -a /builder/addons/rtos-firmware/patches/freertos/cvitek/task/CMakeLists.txt $(RTOS_FIRMWARE_SDK)/freertos/cvitek/task/CMakeLists.txt
	@cp -a /builder/addons/rtos-firmware/patches/freertos/cvitek/driver/gpio/include/gpio.h $(RTOS_FIRMWARE_SDK)/freertos/cvitek/driver/gpio/include/gpio.h
	@cp -a /builder/addons/rtos-firmware/patches/freertos/cvitek/driver/gpio/src/gpio.c $(RTOS_FIRMWARE_SDK)/freertos/cvitek/driver/gpio/src/gpio.c
	@cp -a /builder/addons/rtos-firmware/patches/freertos/cvitek/driver/rtos_cmdqu.h $(RTOS_FIRMWARE_SDK)/freertos/cvitek/driver/rtos_cmdqu.h
	@cp -a /builder/addons/rtos-firmware/patches/freertos/cvitek/driver/rtos_cmdqu/include/rtos_cmdqu.h $(RTOS_FIRMWARE_SDK)/freertos/cvitek/driver/rtos_cmdqu/include/rtos_cmdqu.h
	@cp -a /builder/addons/rtos-firmware/patches/cvi_mpi/include/rtos_cmdqu.h $(RTOS_FIRMWARE_SDK)/cvi_mpi/include/rtos_cmdqu.h
	@cd $(RTOS_FIRMWARE_SDK) && bash -lc ' \
		export PATH=/host-tools/gcc/riscv64-elf-x86_64/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:$$PATH; \
		source build/envsetup_milkv.sh milkv-duo256m-musl-riscv64-sd >/dev/null; \
		build_rtos'
	@test -s $(RTOS_FIRMWARE_ELF)
	@test -s $(RTOS_FIRMWARE_BIN)
	@sha256sum $(RTOS_FIRMWARE_ELF) $(RTOS_FIRMWARE_BIN)
	@touch $@

$(BUILDDIR)/rtos-firmware-stamp: $(BUILDDIR)/rtos-firmware-build-stamp
	@echo "$(COLOUR_GREEN)Installing RTOS firmware for $(BOARD)$(END_COLOUR)"
	@mkdir -p /rootfs/lib/firmware
	@cp -a $(RTOS_FIRMWARE_ELF) /rootfs/lib/firmware/c906-mcu.elf
	@cp -a $(RTOS_FIRMWARE_ELF) /output/$(BOARD)_c906-mcu.elf
	@cp -a $(RTOS_FIRMWARE_BIN) /output/$(BOARD)_c906-mcu.bin
	@mkdir -p /rootfs/usr/bin/
	@/host-tools/gcc/riscv64-linux-musl-x86_64/bin/riscv64-unknown-linux-musl-gcc \
		-O2 -static -Wall -Wextra \
		-I$(RTOS_FIRMWARE_SDK)/cvi_mpi/include \
		-o $(BUILDDIR)/rtos-cmd /builder/addons/rtos-firmware/tools/rtos-cmd.c
	@cp -a $(BUILDDIR)/rtos-cmd /rootfs/usr/bin/rtos-cmd
	@cp -a addons/rtos-firmware/rtos-mode /rootfs/usr/bin/
	@chmod +x /rootfs/usr/bin/rtos-cmd
	@chmod +x /rootfs/usr/bin/rtos-mode
	@touch $@
