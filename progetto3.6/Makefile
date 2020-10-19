INCDIR	= ./include
CC	= gcc
CFLAGS	= -pthread -Wall
TARGET = ./bin/sup

TESTDIR	= ./test
CONFIG	= config.txt


OBJ	= ./src/sup.o ./src/queue.o

.PHONY: test clean

./src/%.o: ./src/%.c $(INCDIR)/queue.h
	$(CC) $(CFLAGS) -c -o $@ $<

all: $(TARGET)

./bin/sup: $(OBJ) $(INCDIR)/queue.h
	$(CC) $(CFLAGS)  -o $@ $(OBJ)

test:
	(./bin/sup -c $(TESTDIR)/config.txt & echo $$! > sup.PID) &
	sleep 25s; \
	kill -1 $$(cat sup.PID); \
	chmod +x ./analisi.sh 
	./analisi.sh $$(cat sup.PID); \

clean:
	rm -f ./bin/sup ./stats.log ./sup.PID
	