OPT=		-O
CFLAGS=		-g $(OPT) -Wall -DDEBUG -DUSE_READLINE #-DUSE_GC
LOADLIBES=	-lreadline #-lgc

TARGET=		mfck
DESTBIN=	/usr/local/bin

$(TARGET):

install:	$(TARGET)
	install -c $(TARGET) $(DESTBIN)

clean:
	rm -rf $(TARGET) *.o
