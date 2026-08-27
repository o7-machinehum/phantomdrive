CC = gcc
CFLAGS = -Wall -Wextra -O2
LDLIBS = -lcrypto -lssl
BUILD_DIR = build
TESTS = sha256_test pbkdf2_test ctr_test xts_test unlock_ctr_test unlock_xts_test
KDF_ROUNDS ?= 100000

ifeq ($(KDF_ROUNDS),100000)
KDF_MODE = 100K
else ifeq ($(KDF_ROUNDS),600000)
KDF_MODE = 600K
else
$(error KDF_ROUNDS must be 100000 or 600000)
endif

CFLAGS += -DKDF_ROUNDS=$(KDF_ROUNDS)U
OBJ_DIR = $(BUILD_DIR)/$(KDF_MODE)

all: $(TESTS)

$(TESTS): %: $(BUILD_DIR)/%

$(BUILD_DIR)/sha256_test: $(OBJ_DIR)/sha256_test.o $(OBJ_DIR)/crypto.o
	$(CC) $^ $(LDLIBS) -o $@

$(BUILD_DIR)/pbkdf2_test: $(OBJ_DIR)/pbkdf2_test.o $(OBJ_DIR)/crypto.o
	$(CC) $^ $(LDLIBS) -o $@

$(BUILD_DIR)/ctr_test: $(OBJ_DIR)/ctr_test.o $(OBJ_DIR)/crypto.o
	$(CC) $^ $(LDLIBS) -o $@

$(BUILD_DIR)/xts_test: $(OBJ_DIR)/xts_test.o $(OBJ_DIR)/crypto.o
	$(CC) $^ $(LDLIBS) -o $@

# Static DMA buffers must fit the firmware's 32-bit address arguments on a host.
UNLOCK_FLAGS = $(CFLAGS) -Werror -Wno-pointer-to-int-cast -fno-pie -no-pie \
               -Istubs -I../src -I../wch-ch56x-bsp/drv \
               -I../wch-ch56x-bsp/usb/usb_devbulk
UNLOCK_SRCS = unlock_test.c ../src/phantomdrive.c ../src/crypto.c \
              ../src/bot.c ../src/scsi_tables.c
UNLOCK_HEADERS = $(wildcard ../src/*.h) stubs/CH56x_common.h

$(BUILD_DIR)/unlock_ctr_test: $(UNLOCK_SRCS) ../src/phantomdrive_aes_ctr.c $(UNLOCK_HEADERS) | $(OBJ_DIR)
	$(CC) $(UNLOCK_FLAGS) $(UNLOCK_SRCS) ../src/phantomdrive_aes_ctr.c $(LDLIBS) -o $@

$(BUILD_DIR)/unlock_xts_test: $(UNLOCK_SRCS) ../src/phantomdrive_aes_xts.c $(UNLOCK_HEADERS) | $(OBJ_DIR)
	$(CC) $(UNLOCK_FLAGS) $(UNLOCK_SRCS) ../src/phantomdrive_aes_xts.c $(LDLIBS) -o $@

$(OBJ_DIR)/%.o: %.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/crypto.o: ../src/crypto.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR):
	mkdir -p $@

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all clean $(TESTS) $(addprefix $(BUILD_DIR)/,$(TESTS))
