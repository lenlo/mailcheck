#
#	Makefile for mfck
#
#	Copyright (c) 2008-2010 by Lennart Lovstrand <mfck@lenlolabs.com>
#
#	Add -DUSE_READLINE to the CFLAGS and -lreadline to LOADLIBES if you
#	have the readline library available.
#
#	There's probably no good reason to add -DUSE_GC & -lgc for now.
#	It's experimental and the code should run fine without it.
#

OPT=		-O
CFLAGS=		-g $(OPT) -Wall -DDEBUG -DUSE_READLINE # -DUSE_GC
LOADLIBES=	-lreadline # -lgc

TARGET=		mfck
DESTBIN=	/usr/local/bin

$(TARGET):

install:	$(TARGET)
	install -c $(TARGET) $(DESTBIN)

clean:
	rm -rf $(TARGET) $(TARGET).tar.gz *.o TAGS

tags:
	etags $(TARGET).c

tar pkg package:
	rev=$$(awk '/const char gRevision/ {print $$6}' $(TARGET).c); \
	mkdir $(TARGET)-$$rev; \
	cp -p $(TARGET).c Makefile $(TARGET)-$$rev; \
	tar -zcf $(TARGET).tar.gz $(TARGET)-$$rev; \
	rm -rf $(TARGET)-$$rev
