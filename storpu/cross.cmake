set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(CMAKE_C_COMPILER aarch64-none-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-none-linux-gnu-g++)
set(CMAKE_ASM_COMPILER aarch64-none-linux-gnu-gcc)
set(CMAKE_AR aarch64-none-linux-gnu-ar)

SET(ASM_OPTIONS "-x assembler-with-cpp -D__ASSEMBLY__")
SET(CMAKE_ASM_FLAGS "${CFLAGS} ${ASM_OPTIONS}")
