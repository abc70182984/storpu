LIB = storpu
SRCS = errno.c memcpy.S memset.S sbrk.c mmap.c malloc.c fs.c

CROSS = aarch64-none-linux-gnu-
CC = $(CROSS)gcc
AS = $(CROSS)as
AR = ar

CFLAGS = -fPIC -I./include
ARFLAGS = rcs

SRC_PATH = ./src
BUILD_PATH = ./obj
OBJS = $(patsubst %.c, $(BUILD_PATH)/%.o, $(patsubst %.S, $(BUILD_PATH)/%.o, $(patsubst %.asm, $(BUILD_PATH)/%.o, $(SRCS))))
DEPS = $(OBJS:.o=.d)
Q = @

BIN = $(BUILD_PATH)/lib$(LIB).a

.PHONY : everything all clean realclean install

all : realclean $(BUILD_PATH) $(BIN) libtest.so
	@true

everything : $(BUILD_PATH) $(BIN)
	@true

clean :
	$(Q)rm -f $(BIN)

realclean :
	$(Q)rm -f $(BIN) $(OBJS)
	$(Q)rm -f libtest.so test.o

install :
	$(Q)$(INSTALL) $(BIN) $(DESTDIR)$(INSTALL_PREFIX)

$(BIN): $(OBJS) $(EXTRAOBJS)
	@echo -e '\tAR\t$@'
	$(Q)$(AR) $(ARFLAGS) $@ $(OBJS) $(EXTRAOBJS)

$(BUILD_PATH):
	$(Q)mkdir $(BUILD_PATH)

-include $(DEPS)

$(BUILD_PATH)/%.o: $(SRC_PATH)/%.c
	@echo -e '\tCC\t$<'
	$(Q)$(CC) $(CFLAGS) -c -MP -MMD $< -o $@

$(BUILD_PATH)/%.o: $(SRC_PATH)/%.S
	@echo -e '\tAS\t$<'
	$(Q)$(CC) $(CFLAGS) -c -D__ASSEMBLY__ -o $@ $<

libtest.so: test.c
	$(CC) $(CFLAGS) -c -fPIC test.c -o test.o
	$(CC) $(CFLAGS) -shared -nostdlib -L./obj -o libtest.so test.o -lstorpu
	#cp libtest.so $(HOME)/vitis_workspace/storpu_test/Debug/
	#touch ../src/payload.S
	#rm -f $(HOME)/vitis_workspace/storpu_test/Debug/payload.o
	nm libtest.so | grep load_data
	nm libtest.so | grep test
	nm libtest.so | grep data_read
