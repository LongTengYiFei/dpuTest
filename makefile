SRC = cyf_test_main.c cyf_test_sample.c ../../common.c ../../../applications/common/src/utils.c
INC = -I/opt/mellanox/doca/include \
	  -I/opt/mellanox/doca/samples \
	  -I/opt/mellanox/doca/applications/common/src 
LIBS = -ldoca_sha -ldoca_common -ldoca_argp -L/opt/mellanox/doca/lib/aarch64-linux-gnu/

doca:
	gcc $(SRC) $(INC) $(LIBS) -o doca_test

arm_sha:
	gcc arm_cpu_sha_test.c -lcrypto -o arm_sha_test

arm_deflate:
	gcc arm_cpu_deflate_test.c -lz -o arm_deflate_test

clean:
	rm arm_sha_test
	rm arm_deflate_test
	rm doca_test