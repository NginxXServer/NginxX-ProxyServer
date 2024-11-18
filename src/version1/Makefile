CC = gcc
CFLAGS = -Wall

SRC_DIR = src/version1
BIN_FILE = reverseProxy

all: $(BIN_FILE)

$(BIN_FILE): $(SRC_DIR)/*.c
	$(CC) $(CFLAGS) -o $(BIN_FILE) $(SRC_DIR)/*.c

clean:
	rm -f $(BIN_FILE)