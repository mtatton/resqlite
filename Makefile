all:
	gcc -O2 -fPIC -shared -DSQLITE_ENABLE_PREUPDATE_HOOK resqlite.c -lsqlite3 -o resqlite.so

clean:
	rm resqlite.so
