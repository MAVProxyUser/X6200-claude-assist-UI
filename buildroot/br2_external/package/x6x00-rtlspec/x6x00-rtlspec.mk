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
X6X00_RTLSPEC_DEPENDENCIES = libusb rtlsdr-blog libhackrf libbladerf zlib

# The sources: tools/rtlspec/daemon_sources, the list tools/rtlsdr/build.sh compiles for a hot install too (the two
# lists were apart once: the image's had no lrpt.c). libusb's header too: sdr_rtl.c reads libusb's port list to
# number the RTLs by serial and USB location. zlib: the Meteor pictures' PNGs (rtlspec.c)
X6X00_RTLSPEC_SOURCES = $(addprefix $(@D)/,$(shell sed -e '/^\#/d' $(@D)/daemon_sources))
# x6x00-df, the KrakenSDR's direction finder (page I): its own repository (github.com/MAVProxyUser/x6x00-df), the
# submodule tools/rtlspec/x6x00-df, compiled from its df_sources; librtlsdr linked in the same way. NEON asked for
# (both radios' SoCs, Cortex-A7, have it): df_chan.c's filter is written for it, plain C otherwise
X6X00_RTLSPEC_DF = $(@D)/x6x00-df
X6X00_RTLSPEC_DF_SOURCES = $(addprefix $(X6X00_RTLSPEC_DF)/,$(shell sed -e '/^\#/d' $(X6X00_RTLSPEC_DF)/df_sources))

define X6X00_RTLSPEC_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -DSDR_BLADERF -I$(@D) -I$(STAGING_DIR)/usr/include/libhackrf \
		-I$(STAGING_DIR)/usr/include/libusb-1.0 \
		-o $(@D)/x6x00-rtlspec $(X6X00_RTLSPEC_SOURCES) \
		-Wl,-Bstatic -lrtlsdr -lhackrf -Wl,-Bdynamic -lusb-1.0 -lz -lpthread -lm -ldl
	@test -f $(X6X00_RTLSPEC_DF)/df_sources || { echo "x6x00-df: the submodule tools/rtlspec/x6x00-df is empty:" \
		"git submodule update --init tools/rtlspec/x6x00-df"; exit 1; }
	$(TARGET_CC) $(TARGET_CFLAGS) -O3 -mfpu=neon-vfpv4 $(TARGET_LDFLAGS) -I$(X6X00_RTLSPEC_DF) \
		-I$(STAGING_DIR)/usr/include/libusb-1.0 -o $(@D)/x6x00-df $(X6X00_RTLSPEC_DF_SOURCES) \
		-Wl,-Bstatic -lrtlsdr -Wl,-Bdynamic -lusb-1.0 -lpthread -lm
endef

define X6X00_RTLSPEC_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/x6x00-rtlspec $(TARGET_DIR)/usr/bin/x6x00-rtlspec
	$(INSTALL) -D -m 0755 $(@D)/x6x00-df $(TARGET_DIR)/usr/bin/x6x00-df
endef

$(eval $(generic-package))
