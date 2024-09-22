SRC = compress_deflate_main.c compress_deflate_sample.c compress_common.c /opt/mellanox/doca/samples/common.c /opt/mellanox/doca/applications/common/utils.c
SRC1 = cpu_test.c
SRC2 = cpu_test2.c
INC = -I/opt/mellanox/doca/include \
	  -I/opt/mellanox/doca/samples \
	  -I/opt/mellanox/doca/applications/common/

LIBS = -ldoca_compress -ldoca_common -ldoca_argp -lm -lz\
	   -L/opt/mellanox/doca/lib/x86_64-linux-gnu/

doca_test:
	gcc $(SRC) $(INC) $(LIBS) -o doca_compress_test -g

cpu_test1:
	gcc $(SRC1) $(INC) $(LIBS) -o cpu_compress_test1 -g

cpu_test2:
	gcc $(SRC2) $(INC) $(LIBS) -o cpu_compress_test2 -g