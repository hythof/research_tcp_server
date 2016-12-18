run: compile
	@./bin/rts

compile:
	@mkdir -p bin
	@gcc -O3 -std=c11 -Wall -W -D_POSIX_C_SOURCE=201112L src/*.c -o bin/rts

benchmark: compile
	@./bin/rts &
	@-ab -q -n 10000 -c 1000 http://localhost:8880/ | grep Request
	@pkill -USR1 rts

benchmark_go:
	@go run benchmark/httpd.go &
	@sleep 1 # wait for spinup
	@-ab -q -n 10000 -c 1000 http://localhost:8880/ | grep Request
	@pkill httpd

longrun: compile
	@./bin/rts &
	@ps u | head -n 1
	@ps u | grep bin/rts | grep -v grep
	@-ab -q -n 1 -c 1 http://localhost:8880/ > /dev/null
	@ps u | grep bin/rts | grep -v grep
	@-ab -q -n 1000 -c 1000 http://localhost:8880/ > /dev/null
	@ps u | grep bin/rts | grep -v grep
	@-ab -q -n 10000 -c 1000 http://localhost:8880/ > /dev/null
	@ps u | grep bin/rts | grep -v grep
	@-ab -q -n 10000 -c 1000 http://localhost:8880/ > /dev/null
	@ps u | grep bin/rts | grep -v grep
	@-ab -q -n 10000 -c 1000 http://localhost:8880/ > /dev/null
	@ps u | grep bin/rts | grep -v grep
	@pkill -USR1 rts
