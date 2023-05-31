cmake_minimum_required(VERSION 3.16)

project(storpu)

enable_language(C ASM)

set(TOPDIR ${PROJECT_SOURCE_DIR})

include_directories(${TOPDIR}/include)

set(LIBSTORPU_SRCLIST
        src/string/memcpy.S
        src/string/memset.S
        src/string/memcmp.S
        src/string/memchr.S
        src/string/strcmp.S
        src/string/strlen.S
        src/string/strchr.S
        src/string/strrchr.S
        src/string/strncmp.S
        src/string/strnlen.S
        src/errno.c
        src/malloc.c
        src/mmap.c
        src/sbrk.c
        src/fs.c)

add_library(storpu STATIC ${LIBSTORPU_SRCLIST})
set_property(TARGET storpu PROPERTY POSITION_INDEPENDENT_CODE ON)

set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/lib)

if (IS_DIRECTORY "${TOPDIR}/local")
   add_subdirectory(local)
endif()

file(GLOB OUTPUT_LIBRARIES "${PROJECT_BINARY_DIR}/lib/*.so")
foreach (output_lib ${OUTPUT_LIBRARIES})
        get_filename_component(lib_name ${output_lib} NAME_WE)

        add_custom_target(
                generate_${lib_name}_symbols ALL
                DEPENDS
                    ${PROJECT_BINARY_DIR}/lib/${lib_name}_symbols.h
                    ${lib_name}
        )

        add_custom_command(
                OUTPUT
                    ${PROJECT_BINARY_DIR}/lib/${lib_name}_symbols.h
                DEPENDS ${output_lib}
                COMMAND python ${TOPDIR}/scripts/gen_symbols.py ${output_lib} ${PROJECT_BINARY_DIR}/lib/${lib_name}_symbols.h
        )
endforeach()
