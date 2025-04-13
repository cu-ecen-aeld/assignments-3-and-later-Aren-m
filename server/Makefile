CC = $(CROSS_COMPILE)gcc
CFLAGS = -g -Wall -Werror
TARGET = aesdsocket
SRC = aesdsocket.c
OBJ = $(SRC:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@
	
clean:
	rm -f $(TARGET) $(OBJ)
