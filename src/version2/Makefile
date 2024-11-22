CC = gcc
CFLAGS = -Wall
SRC_DIR = .
PROXY_DIR = proxy
UTILS_DIR = utils

SRC_FILES = main.c \
           $(PROXY_DIR)/proxy.c \
           $(UTILS_DIR)/logger.c

BIN_FILE = reverseProxy

all: $(BIN_FILE)

$(BIN_FILE): $(SRC_FILES)
	$(CC) $(CFLAGS) -o $(BIN_FILE) $(SRC_FILES)

clean:
	rm -f $(BIN_FILE)