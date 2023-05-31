include_directories(pgtest/storpupg)

set(SOURCE_FILES
    main.cpp
)

set(EXT_SOURCE_FILES
    ${PROTO_SRCS}
)

set(LIBRARIES
    pthread
    atomic
    CONAN_PKG::spdlog
    CONAN_PKG::cxxopts
    unvme
    mcmq    
)

add_executable(storpu-test ${SOURCE_FILES} ${EXT_SOURCE_FILES})
target_link_libraries(storpu-test ${LIBRARIES})

set(PGTEST_BACKEND "mcmq" CACHE STRING "" FORCE)
add_subdirectory(pgtest)

add_executable(pgtest-mcmq pgtest-mcmq.cpp  ${EXT_SOURCE_FILES})
target_link_libraries(pgtest-mcmq pgtest ${LIBRARIES})

add_executable(pgtest-storpu pgtest-storpu.cpp  ${EXT_SOURCE_FILES})
target_link_libraries(pgtest-storpu pgtest ${LIBRARIES})

add_executable(synthetic-mcmq synthetic-mcmq.cpp  ${EXT_SOURCE_FILES})
target_link_libraries(synthetic-mcmq ${LIBRARIES})

add_executable(synthetic-storpu synthetic-storpu.cpp  ${EXT_SOURCE_FILES})
target_link_libraries(synthetic-storpu ${LIBRARIES})

add_executable(generate-knn generate-knn.cpp  ${EXT_SOURCE_FILES})
target_link_libraries(generate-knn ${LIBRARIES})

add_executable(generate-grep generate-grep.cpp  ${EXT_SOURCE_FILES})
target_link_libraries(generate-grep ${LIBRARIES})

add_library(unvmepg STATIC capi.cpp)
target_link_libraries(unvmepg ${LIBRARIES})
