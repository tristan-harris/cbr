SOURCE := main.c
TARGET := cbr

.PHONY: all install

all: $(TARGET)

$(TARGET): $(SOURCE)
	$(CC) $(SOURCE) -o $@ -std=c99 -Wall -Wextra -Wpedantic

install: $(TARGET)
	cp $(TARGET) /usr/local/bin
