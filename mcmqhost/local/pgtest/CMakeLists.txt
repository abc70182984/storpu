cmake_minimum_required(VERSION 3.11)

project(pgtest)

option(USE_STORPU "Enable StorPU support" OFF)

set(PGTEST_BACKEND "posix" CACHE STRING "Storage backend to use")
set_property(CACHE PGTEST_BACKEND PROPERTY STRINGS "posix" "storpu" "mcmq")

set(TOPDIR ${PROJECT_SOURCE_DIR})

set(LIBPGTEST_SRCLIST
    buffer.c
    index.c    
    heap.c
    btree.c
    fmgr.c
    tuples.c
    data_types.c
    schemas.c
    catalog_agg.c
    varlena.c
    aggregate.c
)

if (NOT PGTEST_BACKEND STREQUAL "storpu")
set(LIBPGTEST_SRCLIST ${LIBPGTEST_SRCLIST}
                      datum.c
                      int.c
                      numeric.c
                      text.c)
endif ()

if (PGTEST_BACKEND STREQUAL "storpu")
add_definitions(-DUSE_STORPU)
set(LIBPGTEST_SRCLIST ${LIBPGTEST_SRCLIST}
                      relation_storpu.c)
elseif (PGTEST_BACKEND STREQUAL "mcmq")
add_definitions(-DUSE_MCMQ)
set(LIBPGTEST_SRCLIST ${LIBPGTEST_SRCLIST}
                      relation_mcmq.cpp)
else ()
set(LIBPGTEST_SRCLIST ${LIBPGTEST_SRCLIST}
                      relation_posix.c)
endif ()

set(LIBRARIES )

add_library(pgtest ${LIBPGTEST_SRCLIST})
set_property(TARGET pgtest PROPERTY POSITION_INDEPENDENT_CODE ON)
target_link_libraries(pgtest ${LIBRARIES})

if (PGTEST_BACKEND STREQUAL "posix")
add_executable(pgtest-exe main.c)
target_link_libraries(pgtest-exe pgtest)
endif()
