SRC = 	doca_ec.c \
		/opt/mellanox/doca/samples/common.c \
		/opt/mellanox/doca/applications/common/utils.c 
SRC2 = test_cpu.c
SRC3 = ec_compare.c \
		/opt/mellanox/doca/samples/common.c

INC = -I/opt/mellanox/doca/include \
	  -I/opt/mellanox/doca/samples \
	  -I/opt/mellanox/doca/applications/common/ \
	  -I/usr/local/include/jerasure

LIBS = -ldoca_erasure_coding -ldoca_common -ldoca_argp -lm \
	   -L/opt/mellanox/doca/lib/x86_64-linux-gnu/

doca_ec_test: $(SRC)
	gcc $(SRC) $(INC) $(LIBS) -o doca_ec_test -g -lstdc++ \
		-lcrypto -L/usr/local/lib

cpu_ec_test: $(SRC2)
	gcc $(SRC2) -o cpu_ec_test -g -lstdc++ -lJerasure -lgf_complete -L/usr/local/lib -I/usr/local/include/jerasure

ec_compare: $(SRC3)
	gcc $(SRC3) $(INC) $(LIBS) -o ec_compare -O2 -g -Wall -Wextra \
		-Wno-deprecated-declarations \
		-lJerasure -lgf_complete -lcrypto -L/usr/local/lib

clean:
	rm -f doca_ec_test cpu_ec_test ec_compare
