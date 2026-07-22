CC = gcc
CFLAGS = -Wall -Wextra -O2
LDLIBS = -lcrypto -lssl
BUILD_DIR = build
TESTS = sha256_test pbkdf2_test ctr_test xts_test

all: $(TESTS)

$(TESTS): %: $(BUILD_DIR)/%

$(BUILD_DIR)/sha256_test: $(BUILD_DIR)/sha256_test.o $(BUILD_DIR)/crypto.o
	$(CC) $^ $(LDLIBS) -o $@

$(BUILD_DIR)/pbkdf2_test: $(BUILD_DIR)/pbkdf2_test.o $(BUILD_DIR)/crypto.o
	$(CC) $^ $(LDLIBS) -o $@

$(BUILD_DIR)/ctr_test: $(BUILD_DIR)/ctr_test.o $(BUILD_DIR)/crypto.o
	$(CC) $^ $(LDLIBS) -o $@

$(BUILD_DIR)/xts_test: $(BUILD_DIR)/xts_test.o $(BUILD_DIR)/crypto.o
	$(CC) $^ $(LDLIBS) -o $@

$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/crypto.o: ../src/crypto.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR):
	mkdir -p $@

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all clean $(TESTS)
