
CC = gcc
CFLAGS = -Wall 

SRC_FILES = main.c proxy/proxy.c
BIN_FILE = reverseProxy

all: $(BIN_FILE)

$(BIN_FILE): $(SRC_FILES)
	$(CC) $(CFLAGS) -o $(BIN_FILE) $(SRC_FILES)

clean:
	rm -f $(BIN_FILE)

