
INC = -I./include  -I/usr/local/include
LIB = /usr/local/lib
LIBS = -lavdevice -lavfilter -lpostproc -lavformat -lavcodec -lswscale -lswresample -lavutil -lpthread -lm -lx264 -lz -lSDL2

all:
#gcc -g tutorial01.c -o tutorial01 $(INC) -ldl -L$(LIB) $(LIBS)
#gcc -g tutorial02.c -o tutorial02 $(INC) -ldl -L$(LIB) $(LIBS) `sdl2-config --cflags --libs`
	gcc -g tutorial03.c -o tutorial03 $(INC) -ldl -L$(LIB) $(LIBS) `sdl2-config --cflags --libs`
clean:
	-rm -f tutorial01
