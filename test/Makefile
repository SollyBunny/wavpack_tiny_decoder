all: build

build: $(patsubst ../%.c,../%.o,$(wildcard ../*.c)) test.o
	g++ -o test $(patsubst ../%.c,../%.o,$(wildcard ../*.c)) test.o -lm \
		-Wl,--gc-sections -ffunction-sections -fdata-sections \
		-lasan

../%.o: ../%.c
	cc -O3 -c $< -o $@

test.o: test.cpp
	g++ -O1 -Wall -Wextra -c test.cpp -o test.o -g -fsanitize=address -fno-omit-frame-pointer

clean:
	rm -f ../*.o test

unusedsymbols: all
	@nm test | awk '/ [Tt] / {print $$3}' | sort -u > .used_symbols.txt
	@for o in ../*.o test.o; do \
		nm $$o | awk '/ [Tt] / {print $$3}' | sort -u > .all_symbols.txt; \
		comm -23 .all_symbols.txt .used_symbols.txt || true; \
	done
	@rm -f .used_symbols.txt .all_symbols.txt

test_most: all
	./test music_menu.wv && mpv out.wav

test_loop: all
	./test sfx_falling_woosh.wv && mpv out.wav
