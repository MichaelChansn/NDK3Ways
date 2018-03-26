LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := jnicrash

LOCAL_SRC_FILES := \
    handler/exception_handler.cpp \
    debuggerd/getevent.c \
    debuggerd/tombstone.c \
    debuggerd/utility.c \
    debuggerd/arm/machine.c \
    corkscrew/ptrace.c \
    corkscrew/backtrace.c \
    corkscrew/demangle.c \
    corkscrew/map_info.c \
    corkscrew/symbol_table.c \
    corkscrew/backtrace-helper.c \
    corkscrew/arch-arm/backtrace-arm.c \
	corkscrew/arch-arm/ptrace-arm.c \
	native_crash_capture.cpp

LOCAL_CFLAGS := -Wall -Wno-unused-parameter -std=gnu99

LOCAL_C_INCLUDES := $(LOCAL_PATH)/cutils

LOCAL_EXPORT_C_INCLUDES := $(LOCAL_C_INCLUDES)

LOCAL_LDLIBS := -llog

include $(BUILD_SHARED_LIBRARY)
