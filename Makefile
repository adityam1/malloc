all:
	gcc -c 618_malloc.c -o 618_malloc.o
	ar  rcs libparmalloc.a      618_malloc.o

clean:
	rm -rf libparmalloc.a
