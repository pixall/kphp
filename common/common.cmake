prepend(COMMON_MAIN_SOURCES ${COMMON_DIR}/
        pipe-utils.cpp
        pid.cpp
        dl-utils-lite.cpp
        server/stats.cpp
        server/statsd-client.cpp
        server/init-binlog.cpp
        server/init-snapshot.cpp

        sha1.cpp
        allocators/freelist.cpp
        algorithms/murmur-hash.cpp
        md5.cpp
        allocators/lockfree-slab.cpp
        server/main-binlog.cpp
        crypto/aes256.cpp
        crypto/aes256-generic.cpp
        crypto/aes256-${CMAKE_SYSTEM_PROCESSOR}.cpp

        fast-backtrace.cpp
        string-processing.cpp
        kphp-tasks-lease/lease-worker-mode.cpp)

prepend(COMMON_KFS_SOURCES ${COMMON_DIR}/kfs/
        kfs.cpp
        kfs-internal.cpp
        kfs-binlog.cpp
        kfs-snapshot.cpp
        kfs-replica.cpp)

prepend(COMMON_TL_SOURCES ${COMMON_DIR}/tl/
        parse.cpp
        query-header.cpp)

prepend(COMMON_TL_METHODS_SOURCES ${COMMON_DIR}/tl/methods/
        rwm.cpp
        string.cpp)

if (NOT CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
    prepend(COMMON_UCONTEXT_SOURCES ${COMMON_DIR}/ucontext/
            darwin/aarch64/context.cpp)
else()
    prepend(COMMON_UCONTEXT_SOURCES ${COMMON_DIR}/ucontext/
            linux/x86_64/startcontext.S
            linux/x86_64/getcontext.S
            linux/x86_64/setcontext.S
            linux/x86_64/swapcontext.S
            linux/x86_64/makecontext.cpp)
    # Unfortunately, not all assemblers supports 64-bit dwarf. The safest way -- disable debug info for asm files
    set_source_files_properties(${COMMON_DIR}/ucontext/linux/x86_64/startcontext.S PROPERTIES COMPILE_FLAGS "-g0")
    set_source_files_properties(${COMMON_DIR}/ucontext/linux/x86_64/getcontext.S PROPERTIES COMPILE_FLAGS "-g0")
    set_source_files_properties(${COMMON_DIR}/ucontext/linux/x86_64/setcontext.S PROPERTIES COMPILE_FLAGS "-g0")
    set_source_files_properties(${COMMON_DIR}/ucontext/linux/x86_64/swapcontext.S PROPERTIES COMPILE_FLAGS "-g0")
endif()

set(COMMON_ALL_SOURCES
    ${COMMON_MAIN_SOURCES}
    ${COMMON_KFS_SOURCES}
    ${COMMON_TL_METHODS_SOURCES}
    ${COMMON_TL_SOURCES}
    ${COMMON_UCONTEXT_SOURCES})

if(COMPILER_CLANG)
    set_source_files_properties(${COMMON_DIR}/string-processing.cpp PROPERTIES COMPILE_FLAGS -Wno-invalid-source-encoding)
endif()

vk_add_library_no_pic(common-src-no-pic OBJECT ${COMMON_ALL_SOURCES})
add_dependencies(common-src-no-pic OpenSSL::no-pic::Crypto RE2::no-pic::re2 ZLIB::no-pic::zlib ZSTD::no-pic::zstd)
target_include_directories(common-src-no-pic PUBLIC ${OPENSSL_NO_PIC_INCLUDE_DIR} ${RE2_NO_PIC_INCLUDE_DIRS} ${ZLIB_NO_PIC_INCLUDE_DIRS} ${ZSTD_NO_PIC_INCLUDE_DIRS})

vk_add_library_pic(common-src-pic OBJECT ${COMMON_ALL_SOURCES})
add_dependencies(common-src-pic OpenSSL::pic::Crypto RE2::pic::re2 ZLIB::pic::zlib ZSTD::pic::zstd)
target_include_directories(common-src-pic PUBLIC ${OPENSSL_PIC_INCLUDE_DIR} ${RE2_PIC_INCLUDE_DIRS} ${ZLIB_PIC_INCLUDE_DIRS} ${ZSTD_PIC_INCLUDE_DIRS})
