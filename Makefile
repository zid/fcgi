all: index.zid

index.zid: index.c fcgi.c fcgi.h
	gcc index.c fcgi.c -o index.zid
