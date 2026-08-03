################################################################################
#
# lua-ove-process
#
################################################################################

LUA_OVE_PROCESS_VERSION = 1
LUA_OVE_PROCESS_SITE = $(LUA_OVE_PROCESS_PKGDIR)/src
LUA_OVE_PROCESS_SITE_METHOD = local
LUA_OVE_PROCESS_LICENSE = MIT
LUA_OVE_PROCESS_LICENSE_FILES = LICENSE
LUA_OVE_PROCESS_DEPENDENCIES = lua

define LUA_OVE_PROCESS_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -fPIC -shared \
		-I$(STAGING_DIR)/usr/include \
		-o $(@D)/process.so $(@D)/ove_process.c \
		$(TARGET_LDFLAGS) -llua
endef

define LUA_OVE_PROCESS_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/process.so \
		$(TARGET_DIR)/usr/lib/lua/$(LUAINTERPRETER_ABIVER)/ove/process.so
endef

$(eval $(generic-package))
