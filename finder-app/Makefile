CROSS_COMPILE ?=

TARGET := writer

all: 
	$(CROSS_COMPILE)gcc -g -Wall -c -o $(TARGET).o $(TARGET).c
	$(CROSS_COMPILE)gcc -g -Wall -I/ $(TARGET).o -o $(TARGET)

clean:
	rm -f $(TARGET) *.o 
