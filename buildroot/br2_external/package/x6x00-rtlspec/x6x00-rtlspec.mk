################################################################################
#
# x6x00-rtlspec: the SDR's daemon for the Sentry's VHF/UHF screens (src/rtl_spectrum.c): an RTL-SDR, a HackRF, a
# bladeRF (libbladeRF opened at run time: its header only at build time). librtlsdr and libhackrf linked in (their
# static archives), so it needs only libusb, which every image has: the update bundle carries it, with libbladeRF.so.2
# (tools/update/mkbundle.py), to radios on an image without them
#
# Built from this repository's tools/rtlspec: buildroot/local-build.sh puts the repository's path in place of
# @OPENX6X00@ below when it copies the package into the tree.
#
################################################################################

X6X00_RTLSPEC_VERSION = local
X6X00_RTLSPEC_SITE = @OPENX6X00@/tools/rtlspec
X6X00_RTLSPEC_SITE_METHOD = local
X6X00_RTLSPEC_LICENSE = LGPL-2.1+
X6X00_RTLSPEC_DEPENDENCIES = rtlsdr-blog libhackrf libbladerf

define X6X00_RTLSPEC_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -DSDR_BLADERF -I$(@D) -I$(STAGING_DIR)/usr/include/libhackrf \
		-o $(@D)/x6x00-rtlspec $(@D)/rtlspec.c $(@D)/same.c $(@D)/rds.c $(@D)/acars.c $(@D)/sdr.c $(@D)/sdr_rtl.c \
		$(@D)/sdr_hackrf.c $(@D)/sdr_bladerf.c $(@D)/sdr_file.c -Wl,-Bstatic -lrtlsdr -lhackrf -Wl,-Bdynamic \
		-lusb-1.0 -lpthread -lm -ldl
endef

define X6X00_RTLSPEC_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/x6x00-rtlspec $(TARGET_DIR)/usr/bin/x6x00-rtlspec
endef

$(eval $(generic-package))
