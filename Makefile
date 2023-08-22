all:
	gcc -g -O1 client.c -o client -lrdmacm -libverbs
	gcc -g -O1 server.c -o server -lrdmacm -libverbs
	gcc queue_tester.c -o queue_tester

# gcc -g -O1 client.c -o client -lrdmacm -libverbs
