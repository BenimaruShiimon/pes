POE_WATCHDOG_VERSION = 1.0
POE_WATCHDOG_SITE = $(BR2_EXTERNAL)/package/poe_watchdog/files
POE_WATCHDOG_SITE_METHOD = local
POE_WATCHDOG_LICENSE = GPL-2.0-or-later

define POE_WATCHDOG_BUILD_CMDS
	$(TARGET_CC) -O2 -Wall -o $(@D)/poe_watchdog $(@D)/poe_watchdog.c -static
endef

define POE_WATCHDOG_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/poe_watchdog $(TARGET_DIR)/usr/sbin/poe_watchdog
	$(INSTALL) -D -m 0755 $(POE_WATCHDOG_PKGDIR)/files/poe_ctl.sh \
		$(TARGET_DIR)/usr/sbin/poe_ctl.sh
	$(INSTALL) -D -m 0644 $(POE_WATCHDOG_PKGDIR)/files/poe_watchdog.conf \
		$(TARGET_DIR)/etc/poe_watchdog.conf
	$(INSTALL) -D -m 0755 $(POE_WATCHDOG_PKGDIR)/files/S99poe_watchdog \
		$(TARGET_DIR)/etc/init.d/S99poe_watchdog
endef

$(eval $(generic-package))
