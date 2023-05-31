add_definitions(-D__UM__)

include_directories(
    include
    ${TOPDIR}/include
    ${TOPDIR}/src
    ${UBDSRV_INCLUDE_DIRS}
)

set(SOURCE_FILES main.c
                 alloc.c
                 ff.c
                 nvme.c
                 slab.c
                 timer.c
                 zdma.c
                 ${TOPDIR}/src/bitmap.c
                 ${TOPDIR}/src/cpu.c
                 ${TOPDIR}/src/iov_iter.c
                 ${TOPDIR}/src/tls.c
                 ${TOPDIR}/src/worker.c
                 ${TOPDIR}/src/coro/attr.c
                 ${TOPDIR}/src/coro/condvar.c
                 ${TOPDIR}/src/coro/mutex.c
                 ${TOPDIR}/src/coro/queue.c
                 ${TOPDIR}/src/coro/rwlock.c
                 ${TOPDIR}/src/coro/scheduler.c
                 ${TOPDIR}/src/coro/thread.c
                 ${TOPDIR}/src/ftl/amu.c
                 ${TOPDIR}/src/ftl/block_manager.c
                 ${TOPDIR}/src/ftl/data_cache.c
                 ${TOPDIR}/src/ftl/ftl.c
                 ${TOPDIR}/src/hdrhistogram/hdr_histogram.c
                 )

set(LIBRARIES m
              pthread
              ${UBDSRV_LIBRARIES}
              CONAN_PKG::liburing)

set(LINKER_SCRIPT ${TOPDIR}/um/linker.ld)
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -T ${LINKER_SCRIPT}")

add_executable(myssd_um ${SOURCE_FILES})
target_link_libraries(myssd_um ${LIBRARIES})
