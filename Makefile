strands: strands.c
	gcc $< -o $@ -ggdb -Wall -lm $$(pkg-config --libs --cflags gtk4)
