SRC = 	main.c ec.c \
		/opt/mellanox/doca/samples/common.c \
		/opt/mellanox/doca/applications/common/utils.c 
SRC2 = test_cpu.c

INC = -I/opt/mellanox/doca/include \
	  -I/opt/mellanox/doca/samples \
	  -I/opt/mellanox/doca/applications/common/ \
	  -I/usr/local/include/jerasure

LIBS = -ldoca_erasure_coding -ldoca_common -ldoca_argp -lm \
	   -L/opt/mellanox/doca/lib/x86_64-linux-gnu/

doca_ec_test:
	gcc $(SRC) $(INC) $(LIBS) -o doca_ec_test -g -lstdc++ \
		-lcrypto -lJerasure -lgf_complete -L/usr/local/lib

cpu_ec_test:
	gcc $(SRC2) -o cpu_ec_test -g -lstdc++ -lJerasure -lgf_complete -L/usr/local/lib -I/usr/local/include/jerasure

clean:
	rm cpu_ec_test