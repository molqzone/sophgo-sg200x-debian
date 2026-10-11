$(BUILDDIR)/aic8800-firmware-stamp:
	@echo "$(COLOUR_GREEN)Installing aic8800-firmware for $(BOARD)$(END_COLOUR)"
	@rm -rf $(BUILDDIR)/aic8800-firmware
	@if [ "$(OFFLINE)" = 1 ]; then test -d "$(OFFLINE_CACHE)/git/firmware.git"; git clone --depth 1 "$(OFFLINE_CACHE)/git/firmware.git" $(BUILDDIR)/aic8800-firmware; else git clone --depth 1 https://github.com/armbian/firmware.git $(BUILDDIR)/aic8800-firmware; fi
	@mkdir -p /rootfs/lib/firmware/aic8800_sdio/aic8800/
	@cp -a $(BUILDDIR)/aic8800-firmware/aic8800/SDIO/aic8800/ /rootfs/lib/firmware/aic8800_sdio/
# 	This is the DUOS firmware
	@cp -a $(BUILDDIR)/aic8800-firmware/aic8800/SDIO/aic8800D80/* /rootfs/lib/firmware/aic8800_sdio/aic8800/
	@touch $@
