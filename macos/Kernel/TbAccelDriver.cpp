/*
 * TbAccelDriver.cpp
 *
 * Intentionally no module entry here. When built as a Kernel Extension target
 * with Xcode, the toolchain generates the kext's kmod_info (and the
 * __realmain/__antimain glue that runs the C++ static initializers under
 * OSMetaClass::preModLoad/postModLoad). Defining KMOD_EXPLICIT_DECL ourselves
 * would duplicate _kmod_info - and hand-rolling that glue is what caused the
 * boot kernel panics in the non-Xcode build.
 *
 * The IOKit personalities in Info.plist do all the work.
 *
 * SPDX-License-Identifier: GPL-2.0
 */
#include <IOKit/IOLib.h>
