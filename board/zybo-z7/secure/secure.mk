ifeq ($(BR2_PACKAGE_ZYBO_SECURE_BOOT),y)
define ZYBO_UBOOT_INSTALL_VERIFIED_BOOT_HEADER
	$(INSTALL) -m 0644 $(BR2_EXTERNAL_ZYBO_PATH)/board/zybo-z7/secure/zybo_verified_boot.h \
		$(@D)/include/zybo_verified_boot.h
endef
UBOOT_POST_PATCH_HOOKS += ZYBO_UBOOT_INSTALL_VERIFIED_BOOT_HEADER

define ZYBO_UBOOT_EXPORT_SIGNING_INPUTS
	mkdir -p $(BINARIES_DIR)/secure-inputs
	$(INSTALL) -m 0644 $(@D)/u-boot-nodtb.bin $(@D)/u-boot.dtb \
		$(BINARIES_DIR)/secure-inputs/
	$(INSTALL) -m 0644 $(@D)/.config $(BINARIES_DIR)/secure-inputs/uboot.config
	$(INSTALL) -m 0755 $(@D)/tools/mkimage $(@D)/tools/fit_check_sign \
		$(BINARIES_DIR)/secure-inputs/
	ln -sf $(TARGET_OBJCOPY) $(BINARIES_DIR)/secure-inputs/arm-objcopy
	ln -sf $(TARGET_LD) $(BINARIES_DIR)/secure-inputs/arm-ld
	ln -sf $(TARGET_NM) $(BINARIES_DIR)/secure-inputs/arm-nm
endef
UBOOT_POST_INSTALL_IMAGES_HOOKS += ZYBO_UBOOT_EXPORT_SIGNING_INPUTS

define ZYBO_LINUX_EXPORT_SIGNING_INPUTS
	mkdir -p $(BINARIES_DIR)/secure-inputs
	$(INSTALL) -m 0644 $(@D)/.config $(BINARIES_DIR)/secure-inputs/linux.config
endef
LINUX_POST_INSTALL_IMAGES_HOOKS += ZYBO_LINUX_EXPORT_SIGNING_INPUTS
endif
