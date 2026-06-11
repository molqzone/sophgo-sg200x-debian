$(BUILDDIR)/sysctl-compat-stamp:
	@echo "$(COLOUR_GREEN)Installing sysctl-compat for $(BOARD)$(END_COLOUR)"
	@mkdir -pv /rootfs/etc/sysctl.d/
	@cp -a addons/sysctl-compat/overlay/etc/sysctl.d/*.conf /rootfs/etc/sysctl.d/
	@touch $@
