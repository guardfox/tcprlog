fetch_logs: fetch_logs.c
	(cd st-1.7 && gmake `uname|awk '{print tolower($$1) "-optimized"}'`)
	gcc -DLINUX -DUSE_POLL -Wall -Ist-1.7/obj -o fetch_logs fetch_logs.c st-1.7/obj/libst.a
clean:
	rm -f fetch_logs
	(cd st-1.7 && gmake clean)

