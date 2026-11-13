################################################################################
#
# rtlsdr-blog: librtlsdr and its tools, the RTL-SDR Blog fork (the R828D of the Blog V4 too)
#
################################################################################

RTLSDR_BLOG_VERSION = aed0ea19f3a273370a13c9009b96313c75d54c7b
RTLSDR_BLOG_SITE = $(call github,rtlsdrblog,rtl-sdr-blog,$(RTLSDR_BLOG_VERSION))
RTLSDR_BLOG_LICENSE = GPL-2.0+
RTLSDR_BLOG_LICENSE_FILES = COPYING
RTLSDR_BLOG_INSTALL_STAGING = YES
RTLSDR_BLOG_DEPENDENCIES = libusb
RTLSDR_BLOG_CONF_OPTS = -DDETACH_KERNEL_DRIVER=ON -DINSTALL_UDEV_RULES=OFF

$(eval $(cmake-package))
