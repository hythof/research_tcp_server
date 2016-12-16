run: compile
	./bin/server

compile:
	mkdir -p bin
	gcc -O3 -std=c11 -Wall -W -D_POSIX_C_SOURCE=201112L -pthread src/*.c -o bin/server

profile:
	mkdir -p bin
	gcc -pg -O0 -std=c11 -Wall -W -D_POSIX_C_SOURCE=201112L -pthread src/*.c -o bin/debug_server
	PROFILE=1 ./bin/debug_server &
	ab -q -n 10000 -c 1000 http://localhost:8880/ | grep Request
	pkill -USR1 debug_server
	gprof ./bin/debug_server gmon.out -p -z
