SOURCE ?= main.c
TARGET ?= cbr

.PHONY: all install

all: $(TARGET)

$(TARGET): $(SOURCE)
	$(CC) $(SOURCE) -o $@ -static -std=c99 -Wall -Wextra -Wpedantic

install: $(TARGET)
	cp $(TARGET) $(HOME)/.local/bin
