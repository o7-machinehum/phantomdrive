CC = gcc
CFLAGS = -Wall -Wextra -O2
LDLIBS = -lcrypto -lssl

all: sha256_test kdf xts_test

sha256_test: sha256_test.o crypto.o
	$(CC) $^ $(LDLIBS) -o $@

kdf: kdf.o crypto.o
	$(CC) $^ $(LDLIBS) -o $@

xts_test: xts_test.o crypto.o
	$(CC) $^ $(LDLIBS) -o $@

sha256_test.o: sha256_test.c
	$(CC) $(CFLAGS) -c $< -o $@

kdf.o: kdf.c
	$(CC) $(CFLAGS) -c $< -o $@

xts_test.o: xts_test.c
	$(CC) $(CFLAGS) -c $< -o $@

crypto.o: ../src/crypto.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f sha256_test kdf xts_test kdf_test *.o

.PHONY: all clean
