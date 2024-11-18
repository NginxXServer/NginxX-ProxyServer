
CC = gcc
CFLAGS = -Wall -Isrc/version1/proxy

SRC_DIR = .
MAIN_SRC = main.c
PROXY_SRC = proxy/proxy.c
SRC_FILES = $(MAIN_SRC) $(PROXY_SRC)
BIN_FILE = reverseProxy

all: $(BIN_FILE)

$(BIN_FILE): $(SRC_FILES)
        $(CC) $(CFLAGS) -o $(BIN_FILE) $(SRC_FILES)

clean:
        rm -f $(BIN_FILE)

.PHONY: all clean
