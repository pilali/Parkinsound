################################################################################
#
# parkinsound-stepgate
#
################################################################################
# Drop this directory into mod-plugin-builder/plugins/package/ and add
#   source "package/parkinsound-stepgate/Config.in"
# to mod-plugin-builder/plugins/package/Config.in (or symlink it).
#
# Bump _VERSION when a new commit on the plugin repo should be picked up;
# the variable must be a tag or full commit hash so that the tarball
# downloaded by Buildroot is reproducible.
################################################################################

PARKINSOUND_STEPGATE_VERSION = 82112f70e518fb26b6e8375124d1d44bf16c2db3
PARKINSOUND_STEPGATE_SITE = $(call github,pilali,Parkinsound,$(PARKINSOUND_STEPGATE_VERSION))
PARKINSOUND_STEPGATE_BUNDLES = parkinsound-stepgate.lv2

PARKINSOUND_STEPGATE_TARGET_MAKE = \
	$(TARGET_MAKE_ENV) $(TARGET_CONFIGURE_OPTS) $(MAKE) -C $(@D)

define PARKINSOUND_STEPGATE_BUILD_CMDS
	$(PARKINSOUND_STEPGATE_TARGET_MAKE)
endef

define PARKINSOUND_STEPGATE_INSTALL_TARGET_CMDS
	$(PARKINSOUND_STEPGATE_TARGET_MAKE) install \
		DESTDIR=$(TARGET_DIR) \
		INSTALL_PATH=/usr/lib/lv2
endef

$(eval $(generic-package))
