SRC = sha_create_main.c sha_create_sample.c sha_create_cdc.cc\
		/opt/mellanox/doca/samples/common.c \
		/opt/mellanox/doca/applications/common/utils.c 
SRC2 = cpu_sha.c
INC = -I/opt/mellanox/doca/include \
	  -I/opt/mellanox/doca/samples \
	  -I/opt/mellanox/doca/applications/common/

LIBS = -ldoca_sha -ldoca_common -ldoca_argp -lm \
	   -L/opt/mellanox/doca/lib/x86_64-linux-gnu/
#      -L/opt/mellanox/doca/lib/aarch64-linux-gnu/

doca_sha_test:
	gcc $(SRC) $(INC) $(LIBS) -o doca_sha_test -g -lstdc++ -lcrypto

cpu_sha_test:
	gcc $(SRC2) -o cpu_sha_test -g -lssl -lcrypto -lm

arm_sha:
	gcc arm_cpu_sha_test.c -lcrypto -o arm_sha_test

arm_deflate:
	gcc arm_cpu_deflate_test.c -lz -o arm_deflate_test

clean:
	rm doca_sha_test