function(my_deploy_dependencies target_name)
    message(STATUS "📦 Deploying DLLs for target: ${target_name}")
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(VCPKG_BIN "${VCPKG_PATH}/installed/x64-windows/debug/bin")
        set(DLLS
            "7zip.dll"
            "boost_filesystem-vc143-mt-gd-x64-1_87.dll"
            "boost_iostreams-vc143-mt-gd-x64-1_87.dll"
            "boost_program_options-vc143-mt-gd-x64-1_87.dll"
            "boost_python311-vc143-mt-gd-x64-1_87.dll"
            "boost_thread-vc143-mt-gd-x64-1_87.dll"
            "bz2d.dll"
            "liblzma.dll"
            "python3_d.dll"
            "python311_d.dll"
            "zlibd1.dll"
        )
    else()
        set(VCPKG_BIN "${VCPKG_PATH}/installed/x64-windows/bin")
        set(DLLS
            "7zip.dll"
            "boost_filesystem-vc143-mt-x64-1_87.dll"
            "boost_iostreams-vc143-mt-x64-1_87.dll"
            "boost_program_options-vc143-mt-x64-1_87.dll"
            "boost_python311-vc143-mt-x64-1_87.dll"
            "boost_thread-vc143-mt-x64-1_87.dll"
            "bz2.dll"
            "liblzma.dll"
            "python3.dll"
            "python311.dll"
            "zlib1.dll"
        )
    endif()
    foreach(dll ${DLLS})
        add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${VCPKG_BIN}/${dll}"
                    "$<TARGET_FILE_DIR:${target_name}>"
            COMMENT "Copying ${dll} to output directory"
            VERBATIM
        )
    endforeach()
endfunction()