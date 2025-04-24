# config.cmake

# 设置 vcpkg 路径
set(VCPKG_PATH "/opt/vcpkg_root/vcpkg" CACHE STRING "Path to vcpkg installation")
set(CMAKE_TOOLCHAIN_FILE "${VCPKG_PATH}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "Vcpkg toolchain file")
if(WIN32)
    set(VCPKG_TARGET_TRIPLET "x64-windows")
elseif(APPLE)
    set(VCPKG_TARGET_TRIPLET "x64-osx")
elseif(UNIX)
    set(VCPKG_TARGET_TRIPLET "x64-linux")
else()
    message(FATAL_ERROR "Unsupported platform.")
endif()

set(CMAKE_PREFIX_PATH "${VCPKG_PATH}/installed/${VCPKG_TARGET_TRIPLET};${CMAKE_PREFIX_PATH}")

# 设置压缩解压缩密码
set(PRJ_PASSWORD "lwh123456" CACHE STRING "Set 7zip password")