# Copyright (c) 2024 TECH VEDA
# Author: Raghu Bharadwaj
# This code is dual-licensed under the MIT License and GPL v2

CLANG ?= clang
LLC ?= llc
LIBBPF_CFLAGS ?= $(shell pkg-config --cflags libbpf 2>/dev/null)
LIBBPF_LDLIBS ?= $(shell pkg-config --libs libbpf 2>/dev/null)

BPF_OBJ := sbd_trace.bpf.o sbd_kprobe.bpf.o
USER_BIN := sbd_trace sbd_kprobe

all: $(BPF_OBJ) $(USER_BIN)

vmlinux.h:
	@if ! command -v bpftool >/dev/null 2>&1; then \
		echo "bpftool is required to generate vmlinux.h" >&2; exit 1; \
	fi
	bpftool btf dump file /sys/kernel/btf/vmlinux format c > vmlinux.h

sbd_trace.bpf.o: sbd_trace.bpf.c vmlinux.h
	$(CLANG) -target bpf -D__TARGET_ARCH_$(shell uname -m | sed 's/x86_64/x86/' | sed 's/aarch64/arm64/') \
		-O2 -g -c $< -o $@

sbd_kprobe.bpf.o: sbd_kprobe.bpf.c vmlinux.h
	$(CLANG) -target bpf -D__TARGET_ARCH_$(shell uname -m | sed 's/x86_64/x86/' | sed 's/aarch64/arm64/') \
		-O2 -g -c $< -o $@

sbd_trace: sbd_trace.c
	$(CLANG) -O2 -g $< -o $@ $(LIBBPF_CFLAGS) $(LIBBPF_LDLIBS) -lelf -lz

sbd_kprobe: sbd_kprobe.c
	$(CLANG) -O2 -g $< -o $@ $(LIBBPF_CFLAGS) $(LIBBPF_LDLIBS) -lelf -lz

clean:
	rm -f $(BPF_OBJ) $(USER_BIN)

.PHONY: all clean
