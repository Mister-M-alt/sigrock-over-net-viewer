# CMake toolchain: cross-compile for the Raspberry Pi (aarch64, Debian 13) from
# an x86-64 host using the Arch `aarch64-linux-gnu-gcc` toolchain and the Pi's
# own rootfs as the sysroot.
#
# Why sysroot-from-Pi: it pins glibc/libstdc++ headers AND the exact libsigrok/
# libsigrokdecode/glib/python builds the target runs, so we never link against a
# newer symbol version than the Pi provides. libstdc++/libgcc are static-linked
# because the Pi ships gcc-14 while the cross-compiler is gcc-16.
#
# Override the sysroot location with:  PI_SYSROOT=/path cmake ...   (env var)

set(CMAKE_SYSTEM_NAME      Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(_cross aarch64-linux-gnu-)
set(CMAKE_C_COMPILER   ${_cross}gcc)
set(CMAKE_CXX_COMPILER ${_cross}g++)

if(DEFINED ENV{PI_SYSROOT})
  set(PI_SYSROOT "$ENV{PI_SYSROOT}")
else()
  get_filename_component(PI_SYSROOT "${CMAKE_CURRENT_LIST_DIR}/../.pi-sysroot" ABSOLUTE)
endif()
set(CMAKE_SYSROOT       "${PI_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH "${PI_SYSROOT}")

# Find programs on the host, but headers/libs/packages only in the sysroot.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# This is a Debian *multiarch* sysroot: libc, crt*.o, the loader and all libs live
# in usr/lib/aarch64-linux-gnu/, which the Arch cross-toolchain does not search by
# default. Point it there explicitly:
#   -B<triplet>          find startup objects (crt1.o/crti.o/crtn.o)
#   -L<triplet>          find libraries at link time
#   -rpath-link<triplet> resolve indirect (AS_NEEDED) shared-lib dependencies
# The lib->usr/lib symlink then lets ld's sysroot-prefixed script paths
# (/lib/aarch64-linux-gnu/{libc.so.6,ld-linux-aarch64.so.1}) resolve.
set(_triplet "${PI_SYSROOT}/usr/lib/aarch64-linux-gnu")
set(CMAKE_C_FLAGS_INIT   "-B${_triplet}")
set(CMAKE_CXX_FLAGS_INIT "-B${_triplet}")

# Bake the C++/gcc runtime in: Pi has older libstdc++ than the cross-compiler.
set(_son_link "-static-libstdc++ -static-libgcc -L${_triplet} -Wl,-rpath-link,${_triplet}")
set(CMAKE_EXE_LINKER_FLAGS_INIT    "${_son_link}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_son_link}")

# Resolve dependencies with pkg-config against the sysroot, never the host's.
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${PI_SYSROOT}")
set(ENV{PKG_CONFIG_LIBDIR}
    "${PI_SYSROOT}/usr/lib/aarch64-linux-gnu/pkgconfig:${PI_SYSROOT}/usr/share/pkgconfig:${PI_SYSROOT}/usr/lib/pkgconfig")
unset(ENV{PKG_CONFIG_PATH})
