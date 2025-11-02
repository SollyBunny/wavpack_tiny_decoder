all: build

build: $(patsubst ../%.c,../%.o,$(wildcard ../*.c)) test.o
	g++ -o test $(patsubst ../%.c,../%.o,$(wildcard ../*.c)) test.o -lm \
		-Wl,--gc-sections -ffunction-sections -fdata-sections

../%.o: ../%.c
	cc -O3 -c $< -o $@

test.o: test.cpp
	g++ -O1 -Wall -Wextra -c test.cpp -o test.o

clean:
	rm -f ../*.o test

unusedsymbols: all
	objdump -t test | grep "F "

run: all
	./test music_menu.wv && mpv out.wav
