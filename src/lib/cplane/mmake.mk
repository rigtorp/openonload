TARGET		:= $(CPLANE_LIB)
MMAKE_TYPE	:= LIB

LIB_SRCS	:= mib.c onload.c version.c
LIB_OBJS	:= $(LIB_SRCS:%.c=$(MMAKE_OBJ_PREFIX)%.o)

ALL		:= $(TARGET)


ifndef MMAKE_NO_RULES

all: $(ALL)

lib: $(TARGET)

clean:
	@$(MakeClean)

$(TARGET): $(LIB_OBJS)
	$(MMakeLinkStaticLib)

endif

######################################################################
# Autogenerated header for checking user/kernel consistency.
#
_CP_INTF_HDRS	:= cplane/cplane.h cplane/mib.h cplane/server.h \
	cplane/ioctl.h cplane/mmap.h cplane/af_unix.h

CP_INTF_HDRS	:= $(_CP_INTF_HDRS:%=$(SRCPATH)/include/%)

ifdef MMAKE_USE_KBUILD
objd	:= $(obj)/
else
objd	:=
endif

$(objd)cp_intf_ver.h: $(CP_INTF_HDRS)
	@echo "  GENERATE $@"
	@md5=$$(cat $(CP_INTF_HDRS) | md5sum | sed 's/ .*//'); \
	echo "#define OO_CP_INTF_VER  \"$$md5\"" >"$@"

$(objd)$(MMAKE_OBJ_PREFIX)version.o: $(objd)cp_intf_ver.h

######################################################
# linux kbuild support
#
ifdef MMAKE_USE_KBUILD
all:
	 $(MAKE) $(MMAKE_KBUILD_ARGS) KBUILD_EXTMOD=$(BUILDPATH)/lib/cplane _module_$(BUILDPATH)/lib/cplane
clean:
	@$(MakeClean)
	rm -f cp_intf_ver.h cplane_lib.o
endif

ifdef MMAKE_IN_KBUILD
LIB_OBJS := $(LIB_SRCS:%.c=%.o)
cplane_lib-y    := $(LIB_OBJS)
obj-m    := cplane_lib.o
endif

