################################################################################
#
# zybo-monitor
#
################################################################################

ZYBO_MONITOR_VERSION = 1.0
ZYBO_MONITOR_SITE = $(BR2_EXTERNAL_ZYBO_PATH)/src/zybo-monitor
ZYBO_MONITOR_SITE_METHOD = local
ZYBO_MONITOR_LICENSE = MIT
ZYBO_MONITOR_DEPENDENCIES = zybo-btn

define ZYBO_MONITOR_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) $(TARGET_CONFIGURE_OPTS) -C $(@D)
endef

define ZYBO_MONITOR_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/zybo-monitor $(TARGET_DIR)/usr/sbin/zybo-monitor
endef

define ZYBO_MONITOR_INSTALL_INIT_SYSV
	$(INSTALL) -D -m 0755 $(ZYBO_MONITOR_PKGDIR)/S90zybo-monitor \
		$(TARGET_DIR)/etc/init.d/S90zybo-monitor
endef

$(eval $(generic-package))
