# config.cmake

# 设置 vcpkg 路径
set(VCPKG_PATH "E:/vcpkg" CACHE STRING "Path to vcpkg installation")
set(CMAKE_TOOLCHAIN_FILE "${VCPKG_PATH}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "Vcpkg toolchain file")
set(CMAKE_PREFIX_PATH "${VCPKG_PATH}/installed/x64-windows;${CMAKE_PREFIX_PATH}")