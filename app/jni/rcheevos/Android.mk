LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := rcheevos

# Hash generation from ROM files is not used by the app: the game hash is
# configured directly, so RC_CLIENT_SUPPORTS_HASH stays undefined.
LOCAL_SRC_FILES := \
    src/rc_client.c \
    src/rc_compat.c \
    src/rc_util.c \
    src/rc_version.c \
    src/rcheevos/alloc.c \
    src/rcheevos/condition.c \
    src/rcheevos/condset.c \
    src/rcheevos/consoleinfo.c \
    src/rcheevos/format.c \
    src/rcheevos/lboard.c \
    src/rcheevos/memref.c \
    src/rcheevos/operand.c \
    src/rcheevos/rc_validate.c \
    src/rcheevos/richpresence.c \
    src/rcheevos/runtime.c \
    src/rcheevos/runtime_progress.c \
    src/rcheevos/trigger.c \
    src/rcheevos/value.c \
    src/rhash/md5.c \
    src/rapi/rc_api_common.c \
    src/rapi/rc_api_info.c \
    src/rapi/rc_api_runtime.c \
    src/rapi/rc_api_user.c

LOCAL_EXPORT_C_INCLUDES := $(LOCAL_PATH)/include
LOCAL_C_INCLUDES := $(LOCAL_PATH)/include $(LOCAL_PATH)/src

LOCAL_CFLAGS := -std=gnu99 -O2
LOCAL_LDLIBS := -lm

include $(BUILD_STATIC_LIBRARY)
