################################################################################
#
# x6x00-rtlspec: an RTL-SDR stick's spectrum for the Sentry's VHF/UHF screen (src/rtl_spectrum.c)
#
# Built from this repository's tools/rtlspec: buildroot/local-build.sh puts the repository's path in place of
# @OPENX6X00@ below when it copies the package into the tree.
#
################################################################################

X6X00_RTLSPEC_VERSION = local
X6X00_RTLSPEC_SITE = @OPENX6X00@/tools/rtlspec
X6X00_RTLSPEC_SITE_METHOD = local
X6X00_RTLSPEC_LICENSE = LGPL-2.1+
X6X00_RTLSPEC_DEPENDENCIES = rtlsdr-blog

define X6X00_RTLSPEC_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -I$(@D) -o $(@D)/x6x00-rtlspec $(@D)/rtlspec.c $(@D)/same.c \
		-lrtlsdr -lpthread -lm
endef

define X6X00_RTLSPEC_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/x6x00-rtlspec $(TARGET_DIR)/usr/bin/x6x00-rtlspec
endef

$(eval $(generic-package))
