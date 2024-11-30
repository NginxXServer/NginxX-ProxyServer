CC = gcc
CFLAGS = -Wall
INCLUDES = -I./proxy -I./utils -I./monitoring -I./thread

SRC_DIR = .
PROXY_DIR = proxy
UTILS_DIR = utils
MONITORING_DIR = monitoring
THREAD_DIR = thread

SRC_FILES = main.c \
           $(PROXY_DIR)/proxy.c \
           $(UTILS_DIR)/logger.c \
           $(MONITORING_DIR)/health.c \
           $(THREAD_DIR)/threadpool.c

BIN_FILE = reverseProxy

all: $(BIN_FILE)

$(BIN_FILE): $(SRC_FILES)
	$(CC) $(CFLAGS) $(INCLUDES) -o $(BIN_FILE) $(SRC_FILES)

clean:
	rm -f $(BIN_FILE)