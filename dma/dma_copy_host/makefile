SRC = dma_copy_host_main.c dma_copy_host_sample.c dma_common.c /opt/mellanox/doca/samples/common.c /opt/mellanox/doca/applications/common/utils.c
INC = -I/opt/mellanox/doca/include \
	  -I/opt/mellanox/doca/samples \
	  -I/opt/mellanox/doca/applications/common/

LIBS = -ldoca_dma -ldoca_common -ldoca_argp -lm \
	   -L/opt/mellanox/doca/lib/x86_64-linux-gnu/

doca_test:
	gcc $(SRC) $(INC) $(LIBS) -o doca_dma_test -g