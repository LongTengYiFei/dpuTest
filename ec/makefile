SRC = 	main.c ec.c \
		/opt/mellanox/doca/samples/common.c \
		/opt/mellanox/doca/applications/common/utils.c 

INC = -I/opt/mellanox/doca/include \
	  -I/opt/mellanox/doca/samples \
	  -I/opt/mellanox/doca/applications/common/

LIBS = -ldoca-erasure-coding -ldoca_common -ldoca_argp -lm \
	   -L/opt/mellanox/doca/lib/x86_64-linux-gnu/

doca_ec_test:
	gcc $(SRC) $(INC) $(LIBS) -o doca_ec_test -g -lstdc++ -lcrypto

clean:
	rm doca_sha_test