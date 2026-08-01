$(BUILDDIR)/maixcam-aic8800-firmware-stamp:
	@echo "$(COLOUR_GREEN)Installing pinned MaixCAM AIC8800 firmware$(END_COLOUR)"
	@rm -rf $(BUILDDIR)/maixcam-aic8800-firmware
	@git clone --filter=blob:none --no-checkout $(AIC8800_FIRMWARE_REPO) $(BUILDDIR)/maixcam-aic8800-firmware
	@cd $(BUILDDIR)/maixcam-aic8800-firmware && git checkout --detach $(AIC8800_FIRMWARE_COMMIT)
	@mkdir -p $(ROOTFS)/lib/firmware/aic8800_sdio/aic8800/
	@cp -a $(BUILDDIR)/maixcam-aic8800-firmware/aic8800/SDIO/aic8800/ $(ROOTFS)/lib/firmware/aic8800_sdio/
# 	This board uses the D80 SDIO variant, but the vendor driver still opens generic filenames.
	@cp -a $(BUILDDIR)/maixcam-aic8800-firmware/aic8800/SDIO/aic8800D80/* $(ROOTFS)/lib/firmware/aic8800_sdio/aic8800/
	@cp -af $(BUILDDIR)/maixcam-aic8800-firmware/aic8800/SDIO/aic8800D80/fw_patch_table_8800d80_u02.bin $(ROOTFS)/lib/firmware/aic8800_sdio/aic8800/fw_patch_table.bin
	@cp -af $(BUILDDIR)/maixcam-aic8800-firmware/aic8800/SDIO/aic8800D80/fw_patch_8800d80_u02.bin $(ROOTFS)/lib/firmware/aic8800_sdio/aic8800/fw_patch.bin
	@cp -af $(BUILDDIR)/maixcam-aic8800-firmware/aic8800/SDIO/aic8800D80/fw_adid_8800d80_u02.bin $(ROOTFS)/lib/firmware/aic8800_sdio/aic8800/fw_adid.bin
	@cp -af $(BUILDDIR)/maixcam-aic8800-firmware/aic8800/SDIO/aic8800D80/fmacfw_8800d80_u02.bin $(ROOTFS)/lib/firmware/aic8800_sdio/aic8800/fmacfw.bin
	@cp -af $(BUILDDIR)/maixcam-aic8800-firmware/aic8800/SDIO/aic8800D80/lmacfw_rf_8800d80_u02.bin $(ROOTFS)/lib/firmware/aic8800_sdio/aic8800/fmacfw_rf.bin
	@cp -af $(BUILDDIR)/maixcam-aic8800-firmware/aic8800/SDIO/aic8800D80/aic_userconfig_8800d80.txt $(ROOTFS)/lib/firmware/aic8800_sdio/aic8800/aic_userconfig.txt
	@touch $@
