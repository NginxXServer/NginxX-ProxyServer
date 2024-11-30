CC = gcc
CFLAGS = -Wall
INCLUDES = -I./proxy -I./utils -I./monitoring

SRC_DIR = .
PROXY_DIR = proxy
UTILS_DIR = utils
MONITORING_DIR = monitoring

SRC_FILES = main.c \
           $(PROXY_DIR)/proxy.c \
           $(UTILS_DIR)/logger.c \
           $(MONITORING_DIR)/health.c

BIN_FILE = reverseProxy

all: $(BIN_FILE)

$(BIN_FILE): $(SRC_FILES)
	$(CC) $(CFLAGS) $(INCLUDES) -o $(BIN_FILE) $(SRC_FILES)

clean:
	rm -f $(BIN_FILE)