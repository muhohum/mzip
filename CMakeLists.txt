cmake_minimum_required(VERSION 3.20)

project(
    mzip
    VERSION 1.0.0
    DESCRIPTION "Block-sorting lossless compressor"
    HOMEPAGE_URL "https://github.com/muhohum/mzip"
    LANGUAGES CXX
)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(MZIP_TOP_LEVEL ON)
else()
    set(MZIP_TOP_LEVEL OFF)
endif()

option(MZIP_BUILD_CLI "Build the mzip command line tool" ${MZIP_TOP_LEVEL})
option(MZIP_BUILD_TESTS "Build the mzip tests" ${MZIP_TOP_LEVEL})
option(MZIP_INSTALL "Generate install and packaging rules" ${MZIP_TOP_LEVEL})
option(MZIP_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(MZIP_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

function(mzip_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /EHsc)
        if(MZIP_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(
            ${target}
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
                -Wconversion
                -Wsign-conversion
                -Wshadow
        )
        if(MZIP_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

function(mzip_enable_sanitizers target)
    if(MZIP_ENABLE_SANITIZERS)
        if(MSVC)
            message(FATAL_ERROR "MZIP_ENABLE_SANITIZERS is supported with GCC and Clang")
        endif()
        target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    endif()
endfunction()

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

add_library(
    mzip_lib STATIC
    src/codec.cpp
    src/transforms.cpp
)
add_library(mzip::mzip ALIAS mzip_lib)
set_target_properties(mzip_lib PROPERTIES OUTPUT_NAME mzip EXPORT_NAME mzip)

target_compile_features(mzip_lib PUBLIC cxx_std_20)
target_link_libraries(mzip_lib PUBLIC Threads::Threads)
target_include_directories(
    mzip_lib
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)
mzip_set_warnings(mzip_lib)
mzip_enable_sanitizers(mzip_lib)

if(MZIP_BUILD_CLI)
    add_executable(mzip_cli src/cli.cpp)
    set_target_properties(mzip_cli PROPERTIES OUTPUT_NAME mzip)
    target_compile_features(mzip_cli PRIVATE cxx_std_20)
    target_compile_definitions(mzip_cli PRIVATE "MZIP_VERSION=\"${PROJECT_VERSION}\"")
    target_link_libraries(mzip_cli PRIVATE mzip::mzip)
    mzip_set_warnings(mzip_cli)
    mzip_enable_sanitizers(mzip_cli)
endif()

if(MZIP_BUILD_TESTS)
    enable_testing()
    add_executable(mzip_tests tests/test_main.cpp)
    target_compile_features(mzip_tests PRIVATE cxx_std_20)
    target_include_directories(mzip_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(mzip_tests PRIVATE mzip::mzip)
    mzip_set_warnings(mzip_tests)
    mzip_enable_sanitizers(mzip_tests)
    add_test(NAME mzip_tests COMMAND mzip_tests)
endif()

if(MZIP_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    install(
        TARGETS mzip_lib
        EXPORT mzipTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
    install(DIRECTORY include/mzip DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    if(MZIP_BUILD_CLI)
        install(TARGETS mzip_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    endif()

    install(
        EXPORT mzipTargets
        NAMESPACE mzip::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mzip
    )
    configure_package_config_file(
        cmake/mzipConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/mzipConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mzip
    )
    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/mzipConfigVersion.cmake
        COMPATIBILITY SameMajorVersion
    )
    install(
        FILES
            ${CMAKE_CURRENT_BINARY_DIR}/mzipConfig.cmake
            ${CMAKE_CURRENT_BINARY_DIR}/mzipConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mzip
    )

    set(CPACK_PACKAGE_NAME "mzip")
    set(CPACK_PACKAGE_VENDOR "Mukhit Dauirkhan")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
    set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
    set(CPACK_PACKAGE_FILE_NAME
        "${PROJECT_NAME}-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
    if(WIN32)
        set(CPACK_GENERATOR "ZIP")
    else()
        set(CPACK_GENERATOR "TGZ")
    endif()
    include(CPack)
endif()
