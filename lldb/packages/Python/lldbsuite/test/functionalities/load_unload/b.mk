LEVEL := ../../make

LIB_PREFIX := loadunload_

DYLIB_NAME := $(LIB_PREFIX)b
DYLIB_CXX_SOURCES := b.cpp
DYLIB_ONLY := YES

include $(LEVEL)/Makefile.rules
