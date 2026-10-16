# The OpenXR probe: the game's own graphics binding (Direct3D 11 on DXVK on the runtime's Vulkan)
# and its shader compiler, with no game around them. See xr_probe.cpp.
cmake_minimum_required(VERSION 3.22)
project(diablo_vr_xr_probe CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(DXVK_PREFIX "" CACHE PATH "dxvk-native install prefix")
set(VKD3D_PREFIX "/opt/vkd3d" CACHE PATH "vkd3d install prefix")
set(SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../Source")

include(FetchContent)
FetchContent_Declare(OpenXR
  URL https://github.com/KhronosGroup/OpenXR-SDK/archive/refs/tags/release-1.1.63.tar.gz
  URL_HASH SHA256=5b8dac0608f49498fe8e95f562fe6bcf0bc781e561f137348a8c251d06a88b6f
)
set(DYNAMIC_LOADER OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(OpenXR)

add_executable(xr_probe
  xr_probe.cpp
  ${SOURCE_ROOT}/platform/vr/dxvk/xr_d3d11_dxvk.cpp
  ${SOURCE_ROOT}/platform/vr/dxvk/d3dcompiler_vkd3d.cpp)
target_include_directories(xr_probe PRIVATE
  ${SOURCE_ROOT}
  ${SOURCE_ROOT}/platform/vr/dxvk
  ${DXVK_PREFIX}/include/dxvk
  ${VKD3D_PREFIX}/include/vkd3d
  /usr/local/include)
target_compile_options(xr_probe PRIVATE -Wall)
target_link_libraries(xr_probe PRIVATE
  OpenXR::openxr_loader
  ${DXVK_PREFIX}/lib/libdxvk_d3d11.so
  ${DXVK_PREFIX}/lib/libdxvk_dxgi.so
  ${VKD3D_PREFIX}/lib/libvkd3d-shader.a
  m dl pthread)
set_target_properties(xr_probe PROPERTIES BUILD_RPATH "\$ORIGIN/lib" INSTALL_RPATH "\$ORIGIN/lib")
