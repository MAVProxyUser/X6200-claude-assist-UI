################################################################################
#
# dump1090-fa: FlightAware's Mode S decoder for the AIRSPACE screen's RTL-SDR sticks
#
# Not buildroot's own dump1090 package (8.2, installed as dump1090, with osmocom's librtlsdr): this is the version
# the screen was tested with, on the RTL-SDR Blog librtlsdr. ARCH=armv7l picks the ARMv7 NEON DSP flavours whatever
# the build machine is (the Makefile asks uname). -Werror off: the toolchain wrapper's -mcpu=cortex-a7 meets the
# flavours' -march, a warning only.
#
################################################################################

DUMP1090_FA_VERSION = 11.1
DUMP1090_FA_SITE = $(call github,flightaware,dump1090,v$(DUMP1090_FA_VERSION))
DUMP1090_FA_LICENSE = GPL-2.0+
DUMP1090_FA_LICENSE_FILES = COPYING
DUMP1090_FA_DEPENDENCIES = host-pkgconf ncurses rtlsdr-blog

DUMP1090_FA_MAKE_OPTS = \
	UNAME=Linux ARCH=armv7l DUMP1090_VERSION=v$(DUMP1090_FA_VERSION) \
	RTLSDR=yes BLADERF=no HACKRF=no LIMESDR=no SOAPYSDR=no \
	DUMP1090_CFLAGS="-std=c11 -fno-common -Wall -W"

define DUMP1090_FA_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) $(TARGET_CONFIGURE_OPTS) -C $(@D) $(DUMP1090_FA_MAKE_OPTS) dump1090
endef

define DUMP1090_FA_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/dump1090 $(TARGET_DIR)/usr/bin/dump1090-fa
endef

$(eval $(generic-package))
