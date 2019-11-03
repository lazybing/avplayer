
INC = -I./include  -I/usr/local/include
LIB = /usr/local/lib
LIBS = -lavdevice -lavfilter -lpostproc -lavformat -lavcodec -lswscale -lswresample -lavutil -lpthread -lm -lx264 -lz

all:
	gcc -g tutorial01.c -o tutorial01 $(INC) -ldl -L$(LIB) $(LIBS)
clean:
	-rm -f tutorial01
