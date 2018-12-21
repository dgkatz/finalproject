all: webserv
clean:
	-rm -f webserv data.csv histogram.png

webserv: webserv.c
	gcc -Wall webserv.c my_threads.c -o webserv