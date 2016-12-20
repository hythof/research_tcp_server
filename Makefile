CC=gcc -O3 -std=c11 -Wall -W -D_POSIX_C_SOURCE=201112L -I src/lib/ -pthread

test: compile
	@./bin/test

format:
	clang-format -style=Google -i benchmark/*.[ch] test/*.[ch] src/**/*.[ch]

valgrind: compile
	@valgrind --tool=massif ./bin/benchmark &
	@-ab -q -n 4 -c 2 http://localhost:8880/ | grep Request
	@valgrind --tool=callgrind ./bin/benchmark &
	@-ab -q -n 4 -c 2 http://localhost:8880/ | grep Request
	@pkill -USR1 benchmark

benchmark: compile
	@./bin/benchmark &
	@-ab -q -n 10000 -c 1000 http://localhost:8880/ | grep Request
	@pkill -USR1 benchmark

compile:
	@mkdir -p bin
	@${CC} benchmark/main.c src/lib/*.c -o bin/benchmark
	@${CC} test/main.c src/lib/*.c -o bin/test

benchmark_go:
	@go run benchmark/httpd.go &
	@sleep 1 # wait for spinup
	@-ab -q -n 10000 -c 1000 http://localhost:8880/ | grep Request
	@pkill httpd

longrun: compile
	@./bin/benchmark &
	@ps u | head -n 1
	@ps u | grep bin/benchmark | grep -v grep
	@-ab -q -n 1 -c 1 http://localhost:8880/ > /dev/null
	@ps u | grep bin/benchmark | grep -v grep
	@-ab -q -n 1000 -c 1000 http://localhost:8880/ > /dev/null
	@ps u | grep bin/benchmark | grep -v grep
	@-ab -q -n 10000 -c 1000 http://localhost:8880/ > /dev/null
	@ps u | grep bin/benchmark | grep -v grep
	@-ab -q -n 10000 -c 1000 http://localhost:8880/ > /dev/null
	@ps u | grep bin/benchmark | grep -v grep
	@-ab -q -n 10000 -c 1000 http://localhost:8880/ > /dev/null
	@ps u | grep bin/benchmark | grep -v grep
	@pkill -USR1 benchmark
