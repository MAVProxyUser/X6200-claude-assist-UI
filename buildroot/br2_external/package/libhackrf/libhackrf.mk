################################################################################
#
# libhackrf: the HackRF's library alone, for x6x00-rtlspec (tools/rtlspec/sdr_hackrf.c)
#
# hackrf's own release, the library's directory of it built: no tools (buildroot's hackrf package has them, and
# hackrf_transfer transmits), no udev rules (the daemon runs as root). Pinned at the firmware the HackRF here runs;
# tools/rtlsdr/build.sh builds the same tag for a hot install.
#
################################################################################

LIBHACKRF_VERSION = 2024.02.1
LIBHACKRF_SITE = https://github.com/greatscottgadgets/hackrf/releases/download/v$(LIBHACKRF_VERSION)
LIBHACKRF_SOURCE = hackrf-$(LIBHACKRF_VERSION).tar.xz
LIBHACKRF_LICENSE = BSD-3-Clause
LIBHACKRF_LICENSE_FILES = host/libhackrf/src/hackrf.c
LIBHACKRF_SUBDIR = host/libhackrf
LIBHACKRF_INSTALL_STAGING = YES
LIBHACKRF_DEPENDENCIES = libusb
LIBHACKRF_CONF_OPTS = -DINSTALL_UDEV_RULES=OFF

$(eval $(cmake-package))
