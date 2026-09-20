cmake_minimum_required(VERSION 3.20)

project(migrationDaemon LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/criu-include)
file(CREATE_LINK
    ${CMAKE_CURRENT_SOURCE_DIR}/criu/criu/include/version.h
    ${CMAKE_CURRENT_BINARY_DIR}/criu-include/version.h
    SYMBOLIC
)

find_package(libnghttp2-dev REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(REQUIRED IMPORTED_TARGET libnghttp2-dev)

add_executable(migrationDaemon
    main.cpp
    src/criuInterface.cpp
    src/dumpFile.cpp
    src/restorator.cpp
)


target_compile_options(migrationDaemon PRIVATE -fpermissive)

target_include_directories(migrationDaemon PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}/criu-include
    ${CMAKE_CURRENT_SOURCE_DIR}/criu/images
    ${CMAKE_CURRENT_SOURCE_DIR}/criu/lib/c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/containerdInterface
)

target_link_libraries(migrationDaemon PRIVATE
    PkgConfig::libnghttp2-dev
    ${CMAKE_CURRENT_SOURCE_DIR}/criu/lib/c/libcriu.so
    ${CMAKE_CURRENT_SOURCE_DIR}/src/containerdInterface/containerdWrapper.so
    protobuf-c
)
