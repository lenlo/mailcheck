#
#	Makefile for mfck
#
#	Copyright (c) 2008-2025 by Lennart Lovstrand <mfck@lenlolabs.com>
#
#	Add -DUSE_READLINE to the CFLAGS and -lreadline to LOADLIBES if you
#	have the readline library available.
#
#	There's probably no good reason to add -DUSE_GC & -lgc for now.
#	It's experimental and the code should run fine without it.
#

OPT_DEBUG=	-O3 # -DDEBUG
CFLAGS=		-g $(OPT_DEBUG) -Wall -DDEBUG -DUSE_READLINE # -DUSE_GC
LOADLIBES=	-lreadline # -lgc

TARGET=		mfck
DESTBIN=	/usr/local/bin

$(TARGET):	mfck.o md5.o
	$(CC) -o $(TARGET) mfck.o md5.o $(LOADLIBES)

mfck.c:		vers.h

vers.h:		.git/index
	echo "#define kRevision $$(git log --oneline | wc -l)" >$@

install:	$(TARGET)
	install -c $(TARGET) $(DESTBIN)

clean:
	rm -rf vers.h $(TARGET) $(TARGET).tar.gz *.o TAGS

tags:
	etags $(TARGET).c

tar pkg package:
	rev=$$(awk '/const char gRevision/ {print $$6}' $(TARGET).c); \
	mkdir $(TARGET)-$$rev; \
	cp -p $(TARGET).c Makefile $(TARGET)-$$rev; \
	tar -zcf $(TARGET).tar.gz $(TARGET)-$$rev; \
	rm -rf $(TARGET)-$$rev
