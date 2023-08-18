all:
	gcc -g -O1 client07.c -o client -lrdmacm -libverbs

# gcc -g -O1 client.c -o client -lrdmacm -libverbs
