# The PS4 toolchain (DolphinPS4's ps4-love-modern.cmake: PacBrew OpenOrbis + clang/libc++ 21), presented to CMake as
# FreeBSD instead of "Generic", so projects that pick their POSIX code by platform (SDL3: pthreads, unix filesystem
# and time) build their Unix paths, as on the PS5 port. Only for libraries; the eboot links with the plain toolchain.
include($ENV{DOLPHINPS4}/toolchain/ps4-love-modern.cmake)
set(CMAKE_SYSTEM_NAME FreeBSD)
