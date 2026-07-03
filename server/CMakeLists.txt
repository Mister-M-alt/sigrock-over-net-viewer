# sond — Pi-side server. Links libsigrok + libsigrokdecode via pkg-config.
# When cross-compiling, pkg-config is pointed at the Pi sysroot by the toolchain file.

find_package(PkgConfig REQUIRED)
pkg_check_modules(SIGROK REQUIRED IMPORTED_TARGET libsigrok libsigrokdecode)
pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET libzstd)
find_package(Threads REQUIRED)

file(GLOB SERVER_SRC CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
add_executable(sond ${SERVER_SRC})
target_link_libraries(sond PRIVATE son_proto PkgConfig::SIGROK PkgConfig::ZSTD Threads::Threads)
target_compile_options(sond PRIVATE -Wall -Wextra)

install(TARGETS sond RUNTIME DESTINATION bin)
