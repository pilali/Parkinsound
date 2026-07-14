################################################################################
#
# parkinsound-stepgate4
#
################################################################################
# Drop this directory into mod-plugin-builder/plugins/package/ and add
#   source "package/parkinsound-stepgate4/Config.in"
# to mod-plugin-builder/plugins/package/Config.in (or symlink it).
#
# Bump _VERSION when a new commit on the plugin repo should be picked up;
# the variable must be a tag or full commit hash so that the tarball
# downloaded by Buildroot is reproducible. Use `make bump-recipe4` from
# the repository root to point it at the current git HEAD.
################################################################################

PARKINSOUND_STEPGATE4_VERSION = 90c974df0fd5d33c5aaab9fb335d1e87aa800d33
PARKINSOUND_STEPGATE4_SITE = $(call github,pilali,Parkinsound,$(PARKINSOUND_STEPGATE4_VERSION))
PARKINSOUND_STEPGATE4_BUNDLES = parkinsound-stepgate4.lv2

PARKINSOUND_STEPGATE4_TARGET_MAKE = \
	$(TARGET_MAKE_ENV) $(TARGET_CONFIGURE_OPTS) $(MAKE) -C $(@D)

define PARKINSOUND_STEPGATE4_BUILD_CMDS
	$(PARKINSOUND_STEPGATE4_TARGET_MAKE) stepgate4
endef

define PARKINSOUND_STEPGATE4_INSTALL_TARGET_CMDS
	$(PARKINSOUND_STEPGATE4_TARGET_MAKE) install-stepgate4 \
		DESTDIR=$(TARGET_DIR) \
		INSTALL_PATH=/usr/lib/lv2
endef

$(eval $(generic-package))
