# ID6Term - QNX Momentics common.mk
# Builds libID6Term.so (arm/so-le-v7), the iDrive "PluginApp" that puts a shell terminal into the NBT Evo (ID6) HMI.
#
# The library is loaded into HmiMain3D, which is built with GNU libstdc++ (the _gpp C++ library), so this project
# must use the same one (-Y _gpp), never the default Dinkumware _cpp library. It links against copies of the car's
# libAppFramework.A.so / libCoreFramework.A.so in lib/ (from the firmware dump's opt/hmi/ID5/lib) for the
# bmw::PluginApp / bmw::Parcel / bmw::BasicString symbols; those libraries have further dependencies that only
# exist on the head unit, hence --allow-shlib-undefined.

ifndef QCONFIG
QCONFIG=qconfig.mk
endif
include $(QCONFIG)

NAME=ID6Term
USEFILE=

EXTRA_SRCVPATH += $(PROJECT_ROOT)/src
EXTRA_INCVPATH += $(PROJECT_ROOT)/src
EXTRA_LIBVPATH += $(PROJECT_ROOT)/lib

# Next lines are for C++ projects only
EXTRA_SUFFIXES+=cxx cpp
LDFLAGS+=-lang-c++
VFLAG_g=-gstabs+

CCFLAGS += -Y _gpp -O2 -Wall -D_QNX_SOURCE -fno-strict-aliasing -march=armv7-a -mfpu=vfpv3 -mfloat-abi=softfp
LDFLAGS += -Y _gpp -march=armv7-a -mfpu=vfpv3 -mfloat-abi=softfp
LDFLAGS += -L$(PROJECT_ROOT)/lib -lAppFramework.A -lCoreFramework.A -Wl,--allow-shlib-undefined
LDFLAGS += -Wl,-rpath,/opt/hmi/ID5/lib:/lib:/usr/lib

include $(MKFILES_ROOT)/qmacros.mk
ifndef QNX_INTERNAL
QNX_INTERNAL=$(PROJECT_ROOT)/.qnx_internal.mk
endif
include $(QNX_INTERNAL)

# Plain soname (the manifest names "{pluginDir}/ID6Term", the framework dlopens libID6Term.so), not libID6Term.so.1.
SONAME=libID6Term.so

include $(MKFILES_ROOT)/qtargets.mk
OPTIMIZE_TYPE_g=none
OPTIMIZE_TYPE=$(OPTIMIZE_TYPE_$(filter g, $(VARIANTS)))
