# gsdm, the display manager daemon: plain C, links only libc, libXau and
# crypt(3).
include $(GNUSTEP_MAKEFILES)/common.make

CTOOL_NAME = gsdm
gsdm_C_FILES = gsdm.c

# Root-only daemon: installed with the domain's admin tools by default
# (e.g. /usr/GNUstep/System/Tools/Admin, /usr/local/sbin in the Mac layout).
GSDM_INSTALL_DIR ?= $(GNUSTEP_ADMIN_TOOLS)
gsdm_INSTALL_DIR = $(GSDM_INSTALL_DIR)

# Where gsdm reads its configuration (gsdm -config overrides it at run time).
GSDM_CONFIG_FILE ?= /etc/gsdm/gsdm.conf
ADDITIONAL_CFLAGS += -Wall -DGSDM_CONFIG_FILE=\"$(GSDM_CONFIG_FILE)\"

# crypt(3) is in libc on BSD and Darwin and in libcrypt with glibc.
# GSDM_CRYPT_LIBS overrides the guess; GSDM_EXTRA_OBJS adds prebuilt
# objects to the link (a system whose libc does not export crypt can pass
# its own crypt.o here).
ifeq ($(findstring linux, $(GNUSTEP_TARGET_OS)), linux)
GSDM_CRYPT_LIBS ?= -lcrypt
else
GSDM_CRYPT_LIBS ?=
endif
gsdm_OBJ_FILES = $(GSDM_EXTRA_OBJS)
ADDITIONAL_TOOL_LIBS += -lXau $(GSDM_CRYPT_LIBS)
ADDITIONAL_LDFLAGS += $(GSDM_EXTRA_LDFLAGS)

include $(GNUSTEP_MAKEFILES)/ctool.make
