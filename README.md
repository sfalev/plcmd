PLCMD - based on libnodave library daemon which allows to get information from Siemens 300/400 PLCs
PLCM - is web interface to configure PLCMD and operate with data

How to install PLCMD:

$ apt install default-libmysqlclient-dev
$ git clone https://github.com/sfalev/plcmd.git
$ cd plcmd
$ cp libnodave.so /usr/local/lib
$ cp nodave.h /usr/local/include
$ ./make

edit file plcmd.conf with your own params

start daemon with
$ ./plcmd start

stop daemon with
$ ./plcmd stop
