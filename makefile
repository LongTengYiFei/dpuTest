SRC = sha_create_main.c sha_create_sample.c /opt/mellanox/doca/samples/common.c /opt/mellanox/doca/applications/common/utils.c 
SRC2 = x86_test.c
INC = -I/opt/mellanox/doca/include \
	  -I/opt/mellanox/doca/samples \
	  -I/opt/mellanox/doca/applications/common/

LIBS = -ldoca_sha -ldoca_common -ldoca_argp -lm \
	   -L/opt/mellanox/doca/lib/x86_64-linux-gnu/

test:
	gcc $(SRC) $(INC) $(LIBS) -o doca_test -g

test_x86:
	gcc $(SRC2) -o doca_test_x86 -g -lssl -lcrypto 

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