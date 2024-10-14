SRC = main.cc /opt/mellanox/doca/samples/common.c /opt/mellanox/doca/applications/common/utils.c 

INC = -I/opt/mellanox/doca/include \
	  -I/opt/mellanox/doca/samples \
	  -I/opt/mellanox/doca/applications/common/

LIBS = -ldoca_sha -ldoca_common -ldoca_argp -lm \
	   -L/opt/mellanox/doca/lib/x86_64-linux-gnu/

proxy:
	gcc $(SRC) $(INC) $(LIBS) -o proxy -g

clean:
	rm proxy