# mg-skt-cpp

Non-blocking sockets library which can be configured to use
either select() or epoll() I/O multiplexing.

Simple TCP proxy demo included as an example use case:

$ make        # build executable
$ ./tcp-proxy-demo <remote IP address> 127.0.0.1

Now from Chrome web browser, go to "127.0.0.1:8080". It should
render the web page from <remote IP address>.
