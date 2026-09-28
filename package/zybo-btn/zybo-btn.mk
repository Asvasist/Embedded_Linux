################################################################################
#
# zybo-btn
#
################################################################################

ZYBO_BTN_VERSION = 1.0
ZYBO_BTN_SITE = $(BR2_EXTERNAL_ZYBO_PATH)/src/zybo-btn
ZYBO_BTN_SITE_METHOD = local
ZYBO_BTN_LICENSE = GPL-2.0
ZYBO_BTN_INSTALL_STAGING = YES

# event struct header, used by zybo-monitor
define ZYBO_BTN_INSTALL_STAGING_CMDS
	$(INSTALL) -D -m 0644 $(@D)/zybo_btn.h $(STAGING_DIR)/usr/include/zybo_btn.h
endef

$(eval $(kernel-module))
$(eval $(generic-package))
