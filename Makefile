CC = gcc
CFLAGS = -Wall -Wextra
TARGETS = testhttp_raw

all: $(TARGETS)

testhttp_raw.o: testhttp_raw.c err.h

err.o: err.c err.h

testhttp_raw: testhttp_raw.o err.o
	$(CC) $(CFLAGS) $^ -o $@

clean:
	rm -f *.o *~ $(TARGETS)
