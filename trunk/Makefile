OPT=		-O
CFLAGS=		-g $(OPT) -Wall -DDEBUG #-DUSE_GC
LOADLIBES=	#-lgc

TARGET=		mfck
DESTBIN=	/usr/local/bin

$(TARGET):

install:	$(TARGET)
	install -c $(TARGET) $(DESTBIN)

clean:
	rm -rf $(TARGET) *.o
