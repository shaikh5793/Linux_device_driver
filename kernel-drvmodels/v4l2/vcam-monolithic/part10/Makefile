PWD := $(shell pwd)

all:
	gcc -Wall -o test_pipeline test_pipeline.c

clean:
	rm -f test_pipeline

run: all
	@echo "=== Loading modules ==="
	sudo insmod ../hw/vcam_hw_platform.ko 2>/dev/null || true
	sudo insmod ../part7/vcam_expbuf.ko 2>/dev/null || true
	sudo insmod ../part8/buf_reader.ko 2>/dev/null || true
	sudo insmod ../part9/vout_dmabuf.ko 2>/dev/null || true
	@sleep 0.5
	@echo ""
	sudo ./test_pipeline
	@echo ""
	@echo "=== dmesg output ==="
	sudo dmesg | grep -i "vcam\|buf_reader\|vout" | tail -30

.PHONY: all clean run
