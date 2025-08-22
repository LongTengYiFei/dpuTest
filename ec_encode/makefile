SRC = dpuProxy.cc main.cc doca_common.cc

INC = -I/opt/mellanox/doca/include \
	  -I/opt/mellanox/doca/samples \
	  -I/opt/mellanox/doca/applications/common/ \
	  -I/usr/local/include/jerasure/

LIBS = -ldoca_erasure_coding -ldoca_common -ldoca_argp -lm \
	   -L/opt/mellanox/doca/lib/x86_64-linux-gnu/ \
	   -lstdc++ -lcrypto -lJerasure -lgf_complete -L/usr/local/lib -ldoca_sha

dpuProxy:
	gcc $(SRC) $(INC) $(LIBS) -o proxy_test -g  -fno-omit-frame-pointer -O3 -mavx

clean:
	rm proxy_test