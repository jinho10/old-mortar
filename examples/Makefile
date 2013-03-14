
# Mysql Library from mysql_config --libs and mysql_config --cflags
CFLAGS = -I/usr/include/mysql -I../include -DBIG_JOINS=1  -fno-strict-aliasing  -g
LIBS = -L/usr/lib/x86_64-linux-gnu -lmysqlclient -lpthread -lz -lm -lrt -ldl
APP=mcdtest

all: 
	gcc -o $(APP) mcdtest.c $(CFLAGS) $(LIBS)

install:
	cp $(APP) /usr/bin

clean:
	rm $(APP)
