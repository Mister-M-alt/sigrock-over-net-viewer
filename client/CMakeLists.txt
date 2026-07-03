# sonview — local desktop client (Dear ImGui + SDL2 + OpenGL3).

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET libzstd)
set(OpenGL_GL_PREFERENCE GLVND)
find_package(OpenGL REQUIRED)

set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)
add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui PUBLIC
    ${IMGUI_DIR} ${IMGUI_DIR}/backends ${CMAKE_CURRENT_SOURCE_DIR})
# imconfig.h lives in this dir and is on the include path above.
target_compile_definitions(imgui PUBLIC "IMGUI_USER_CONFIG=\"imconfig.h\"")
target_link_libraries(imgui PUBLIC PkgConfig::SDL2)

file(GLOB CLIENT_SRC CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
add_executable(sonview ${CLIENT_SRC})
target_include_directories(sonview PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(sonview PRIVATE
    son_proto imgui PkgConfig::SDL2 PkgConfig::ZSTD OpenGL::GL ${CMAKE_DL_LIBS} pthread)
target_compile_options(sonview PRIVATE -Wall -Wextra)
