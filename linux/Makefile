# Out-of-tree build against the running kernel:
#   make -C /lib/modules/$(uname -r)/build M=$PWD modules
#
# Self-contained: the UAPI header is vendored as thunderbolt_accel.h next to
# this module (the canonical copy lives at include/uapi/linux/ in the kernel
# tree; install it there / to /usr/include/linux for userspace).
obj-m += tbaccel.o
ccflags-y += -I$(src)
