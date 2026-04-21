CC=gcc
CFLAGS=`pkg-config fuse3 --cflags`
LDFLAGS=`pkg-config fuse3 --libs`

all:
	$(CC) mini_unionfs.c -o mini_unionfs $(CFLAGS) $(LDFLAGS)

clean:
	rm -f mini_unionfs