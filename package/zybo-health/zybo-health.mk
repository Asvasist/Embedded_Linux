################################################################################
# zybo-health
################################################################################
ZYBO_HEALTH_VERSION = 1.0
ZYBO_HEALTH_SITE = $(BR2_EXTERNAL_ZYBO_PATH)/src/zybo-health
ZYBO_HEALTH_SITE_METHOD = local
ZYBO_HEALTH_LICENSE = MIT

define ZYBO_HEALTH_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) $(TARGET_CONFIGURE_OPTS) -C $(@D)
endef

define ZYBO_HEALTH_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/zybo-health $(TARGET_DIR)/usr/sbin/zybo-health
endef

define ZYBO_HEALTH_INSTALL_INIT_SYSV
	$(INSTALL) -D -m 0755 $(ZYBO_HEALTH_PKGDIR)/S15zybo-health \
		$(TARGET_DIR)/etc/init.d/S15zybo-health
endef

$(eval $(generic-package))
