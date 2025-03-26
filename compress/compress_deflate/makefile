SRC = compress_deflate_main.c compress_common.c 
SRC += ./doca_common.cc /opt/mellanox/doca/applications/common/utils.c

INC = -I/opt/mellanox/doca/include \
	  -I/opt/mellanox/doca/samples \
	  -I/opt/mellanox/doca/applications/common/

LIBS = -ldoca_compress -ldoca_common -ldoca_argp -lm -lz\
	   -L/opt/mellanox/doca/lib/x86_64-linux-gnu/

doca_test:
	gcc $(SRC) $(INC) $(LIBS) -o doca_compress_test -g -O0