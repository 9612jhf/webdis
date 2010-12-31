OUT=webdis
HIREDIS_OBJ=hiredis/hiredis.o hiredis/sds.o hiredis/net.o hiredis/async.o
JANSSON_OBJ=jansson/src/dump.o jansson/src/error.o jansson/src/hashtable.o jansson/src/load.o jansson/src/strbuffer.o jansson/src/utf.o jansson/src/value.o jansson/src/variadic.o 
FORMAT_OBJS=formats/json.o formats/raw.o
OBJS=webdis.o conf.o $(FORMAT_OBJS) cmd.o server.o $(HIREDIS_OBJ) $(JANSSON_OBJ) libb64/cencode.o 
CFLAGS=-O0 -ggdb -Wall -Wextra -I. -Ijansson/src
LDFLAGS=-levent

all: $(OUT) Makefile

$(OUT): $(OBJS) Makefile
	$(CC) $(LDFLAGS) -o $(OUT) $(OBJS)

%.o: %.c %.h Makefile
	$(CC) -c $(CFLAGS) -o $@ $<

%.o: %.c Makefile
	$(CC) -c $(CFLAGS) -o $@ $<

clean:
	rm -f $(OBJS) $(OUT)

