OUT=radish
OBJS=radish.o hiredis/hiredis.o hiredis/sds.o hiredis/net.o hiredis/async.o
CFLAGS=-O3 -Wall -Wextra -I.
LDFLAGS=-levent
prefix=/usr

all: $(OUT) Makefile

$(OUT): $(OBJS) Makefile
	$(CC) $(LDFLAGS) -o $(OUT) $(OBJS)

%.o: %.c %.h Makefile
	$(CC) -c $(CFLAGS) -o $@ $<

%.o: %.c Makefile
	$(CC) -c $(CFLAGS) -o $@ $<

clean:
	rm -f $(OBJS) $(OUT)

