run: compile
	./bin/rts

compile:
	mkdir -p bin
	gcc -O3 -std=c11 -Wall -W -D_POSIX_C_SOURCE=201112L -pthread src/*.c -o bin/rts

benchmark: compile
	-pkill rts
	./bin/rts &
	sleep 1 # wait for rts
	ab -q -n 10000 -c 1000 http://localhost:8880/ | grep Request
	pkill rts
