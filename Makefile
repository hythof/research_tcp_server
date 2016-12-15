run: compile
	@./bin/rts

compile:
	@mkdir -p bin
	@gcc -O3 -std=c11 -Wall -W -D_POSIX_C_SOURCE=201112L -pthread src/*.c -o bin/rts

benchmark: compile
	@./bin/rts &
	@-ab -q -n 10000 -c 1000 http://localhost:8880/ | grep Request
	@pkill -USR1 rts
	@#go run benchmark/httpd.go &
	@#sleep 1 # wait for spinup
	@#-ab -q -n 10000 -c 1000 http://localhost:8880/ | grep Request
	@#pkill httpd
