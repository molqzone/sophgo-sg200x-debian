$(BUILDDIR)/aic8800-firmware-stamp:
	@echo "$(COLOUR_GREEN)Installing aic8800-firmware for $(BOARD)$(END_COLOUR)"
	@rm -rf $(BUILDDIR)/aic8800-firmware
	@git clone --depth 1 https://github.com/armbian/firmware.git $(BUILDDIR)/aic8800-firmware
	@mkdir -p /rootfs/lib/firmware/aic8800_sdio/aic8800/
	@cp -a $(BUILDDIR)/aic8800-firmware/aic8800/SDIO/aic8800/ /rootfs/lib/firmware/aic8800_sdio/
# 	This board uses the D80 SDIO variant, but the vendor driver still opens generic filenames.
	@cp -a $(BUILDDIR)/aic8800-firmware/aic8800/SDIO/aic8800D80/* /rootfs/lib/firmware/aic8800_sdio/aic8800/
	@cp -af $(BUILDDIR)/aic8800-firmware/aic8800/SDIO/aic8800D80/fw_patch_table_8800d80_u02.bin /rootfs/lib/firmware/aic8800_sdio/aic8800/fw_patch_table.bin
	@cp -af $(BUILDDIR)/aic8800-firmware/aic8800/SDIO/aic8800D80/fw_patch_8800d80_u02.bin /rootfs/lib/firmware/aic8800_sdio/aic8800/fw_patch.bin
	@cp -af $(BUILDDIR)/aic8800-firmware/aic8800/SDIO/aic8800D80/fw_adid_8800d80_u02.bin /rootfs/lib/firmware/aic8800_sdio/aic8800/fw_adid.bin
	@cp -af $(BUILDDIR)/aic8800-firmware/aic8800/SDIO/aic8800D80/fmacfw_8800d80_u02.bin /rootfs/lib/firmware/aic8800_sdio/aic8800/fmacfw.bin
	@cp -af $(BUILDDIR)/aic8800-firmware/aic8800/SDIO/aic8800D80/lmacfw_rf_8800d80_u02.bin /rootfs/lib/firmware/aic8800_sdio/aic8800/fmacfw_rf.bin
	@cp -af $(BUILDDIR)/aic8800-firmware/aic8800/SDIO/aic8800D80/aic_userconfig_8800d80.txt /rootfs/lib/firmware/aic8800_sdio/aic8800/aic_userconfig.txt
	@touch $@
