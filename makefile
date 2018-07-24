CC=g++
COMPILER_FLAGS = -Wall -O3 -std=c++11
LINKER_FLAGS = -I ./include -lnvcuvid -lcuda -lturbojpeg

OBJ_NAME=fasthumb
OBJS=demo.cpp fasthumb.cpp

all:
	$(CC) -o $(OBJ_NAME) $(COMPILER_FLAGS) $(LINKER_FLAGS) $(OBJS)

clean: 
	rm $(OBJ_NAME)
	rm *.o

