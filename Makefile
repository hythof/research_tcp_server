BASE_CC=gcc -std=c11 -Wall -W -D_POSIX_C_SOURCE=201112L -I src/lib/ -pthread
PRODUCT_CC=${BASE_CC} -O2
PROFILE_CC=${BASE_CC} -O2
DEVELOP_CC=${BASE_CC} -O2 -g -fno-inline

test: compile
	@./bin/test

format:
	@clang-format -style=Google -i benchmark/*.[ch] test/*.[ch] src/**/*.[ch]

profile: compile
	@CPUPROFILE=/tmp/cpu.prof LD_PRELOAD=/usr/lib/libprofiler.so.0 ./bin/httpd_profile &
	@-ab -q -n 100000 -c 1000 http://localhost:8880/ | grep Request
	@pkill -USR1 httpd_profile
	@echo "-- gperf"
	@google-pprof --text bin/httpd_profile /tmp/cpu.prof

benchmark: compile
	@./bin/httpd_product &
	@-ab -q -n 10000 -c 1000 http://localhost:8880/ | grep Request
	@pkill -USR1 httpd_product

compile:
	@mkdir -p bin
	@${PRODUCT_CC} benchmark/main.c src/lib/*.c -o bin/httpd_product
	@${DEVELOP_CC} benchmark/main.c src/lib/*.c -o bin/httpd_develop
	@${PROFILE_CC} benchmark/main.c src/lib/*.c -o bin/httpd_profile
	@${DEVELOP_CC} test/main.c src/lib/*.c -o bin/test

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
