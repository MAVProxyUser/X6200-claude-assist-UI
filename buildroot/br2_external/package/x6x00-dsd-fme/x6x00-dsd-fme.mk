################################################################################
#
# x6x00-dsd-fme: dsd-fme (lwvmobile/dsd-fme, its audio_work branch) for the SDR daemon (tools/rtlspec/dsd.c), which
# runs it on B's narrow FM channel for P25, D-STAR, YSF and X2-TDMA. Its voice through DVSI hardware only (the
# owner's rule: no software vocoder): built against libmbe-dvsi (tools/dsdfme/mbe_dvsi, this repository), which
# answers mbelib's calls with the USB-3000 behind the daemon's port, never against mbelib. Our patch
# (tools/dsdfme/patches/goldcorp.patch, copied here as 0001 by buildroot/local-build.sh) sizes its talkgroup table for
# a 32-bit system, reads stdin a buffer at a time, leaves DMR to the daemon (DSD_NO_DMR), says what it hears on a
# pipe (DSD_STATUS_FD), keeps D-STAR's text whole and checks its header's CRC.
#
# buildroot/local-build.sh puts the repository's path in place of @OPENX6X00@ below.
#
################################################################################

X6X00_DSD_FME_VERSION = 33d6cdfb54dffdb5f3ca65eedb6314d716b70645
X6X00_DSD_FME_SITE = https://github.com/lwvmobile/dsd-fme.git
X6X00_DSD_FME_SITE_METHOD = git
X6X00_DSD_FME_LICENSE = ISC, GPL-2.0 (dstar_header.c, descramble.h, fcs.h), GPL (OP25's BCH decoder)
X6X00_DSD_FME_LICENSE_FILES = COPYRIGHT
X6X00_DSD_FME_DEPENDENCIES = libsndfile ncurses pulseaudio host-pkgconf

# libmbe-dvsi as the libmbe dsd-fme's FindMBE takes, with its mbelib.h
define X6X00_DSD_FME_MBE_DVSI
	mkdir -p $(@D)/mbe-dvsi
	$(TARGET_CC) $(TARGET_CFLAGS) -c -o $(@D)/mbe-dvsi/mbe_dvsi.o @OPENX6X00@/tools/dsdfme/mbe_dvsi/mbe_dvsi.c
	$(TARGET_AR) rcs $(@D)/mbe-dvsi/libmbe.a $(@D)/mbe-dvsi/mbe_dvsi.o
	cp @OPENX6X00@/tools/dsdfme/mbe_dvsi/mbelib.h $(@D)/mbe-dvsi/
	# the image's ncurses is the narrow one
	sed -i 's/set(CURSES_NEED_WIDE TRUE)/set(CURSES_NEED_WIDE FALSE)/' $(@D)/CMakeLists.txt
endef
X6X00_DSD_FME_PRE_CONFIGURE_HOOKS += X6X00_DSD_FME_MBE_DVSI

X6X00_DSD_FME_CONF_OPTS = \
	-DMBE_INCLUDE_DIR=$(@D)/mbe-dvsi \
	-DMBE_LIBRARY=$(@D)/mbe-dvsi/libmbe.a \
	-DCOLORS=OFF

$(eval $(cmake-package))
