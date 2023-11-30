SRC = cyf_test_main.c cyf_test_sample.c ../../common.c ../../../applications/common/src/utils.c
INC = -I/opt/mellanox/doca/include \
	  -I/opt/mellanox/doca/samples \
	  -I/opt/mellanox/doca/applications/common/src 
LIBS = -ldoca_sha -ldoca_common -ldoca_argp -L/opt/mellanox/doca/lib/aarch64-linux-gnu/

doca:
	gcc $(SRC) $(INC) $(LIBS) -o doca_test

arm:
	gcc arm_cpu_sha_test.c -lcrypto -o arm_test