################################################################################
#
# x6x00-modem: one end of a modem link between two radios (tools/modem/README.md): BinModem's fax modems in C, over
# the radio's TCI. libc and libm only.
#
# Built from this repository's tools/modem: buildroot/local-build.sh puts the repository's path in place of
# @OPENX6X00@ below when it copies the package into the tree.
#
################################################################################

X6X00_MODEM_VERSION = local
X6X00_MODEM_SITE = @OPENX6X00@/tools/modem
X6X00_MODEM_SITE_METHOD = local
X6X00_MODEM_LICENSE = GPL-3.0+
X6X00_MODEM_SOURCES = x6x00-modem.c dsp.c v21.c v27ter.c v29.c qamrx.c pump.c hdlc.c link.c

define X6X00_MODEM_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -O2 $(TARGET_LDFLAGS) -o $(@D)/x6x00-modem.bin \
		$(addprefix $(@D)/,$(X6X00_MODEM_SOURCES)) -lm
endef

define X6X00_MODEM_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/x6x00-modem.bin $(TARGET_DIR)/usr/bin/x6x00-modem
endef

$(eval $(generic-package))
