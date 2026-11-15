################################################################################
#
# libbladerf: Nuand's libbladeRF alone, for x6x00-rtlspec (tools/rtlspec/sdr_bladerf.c opens it at run time)
#
# The bladeRF repository at its library's tag, with the analogdevicesinc submodule the bladeRF 2.0 needs; its tools
# (bladeRF-cli, bladeRF-update with curl) taken out of the build, as they have no switch. No FPGA images: they are
# Nuand's, fetched and put on the radio by hand (docs/MANUAL.md). tools/rtlsdr/build.sh builds the same tag for a hot
# install.
#
################################################################################

LIBBLADERF_VERSION = libbladeRF_v2.6.0
LIBBLADERF_SITE = https://github.com/Nuand/bladeRF.git
LIBBLADERF_SITE_METHOD = git
LIBBLADERF_GIT_SUBMODULES = YES
LIBBLADERF_LICENSE = LGPL-2.1+
LIBBLADERF_LICENSE_FILES = COPYING legal/licenses/LICENSE.LGPLv2.1
LIBBLADERF_SUBDIR = host
LIBBLADERF_INSTALL_STAGING = YES
LIBBLADERF_DEPENDENCIES = libusb
LIBBLADERF_CONF_OPTS = \
	-DBUILD_DOCUMENTATION=OFF \
	-DENABLE_BACKEND_LIBUSB=ON \
	-DENABLE_BACKEND_DUMMY=OFF \
	-DTEST_LIBBLADERF=OFF \
	-DINSTALL_UDEV_RULES=OFF \
	-DTREAT_WARNINGS_AS_ERRORS=OFF \
	-DLIBUSB_SKIP_VERSION_CHECK=Yes

define LIBBLADERF_NO_UTILITIES
	$(SED) '/^add_subdirectory(utilities)$$/d' $(@D)/host/CMakeLists.txt
endef
LIBBLADERF_POST_PATCH_HOOKS += LIBBLADERF_NO_UTILITIES

$(eval $(cmake-package))
