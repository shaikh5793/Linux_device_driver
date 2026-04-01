CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -O2
LDFLAGS =
EXAMPLES = discover formats controls buffers

all: $(EXAMPLES)

discover: discover.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

formats: formats.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

controls: controls.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

buffers: buffers.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(EXAMPLES)

help:
	@echo "Available targets:"
	@echo "  all           - Build all examples"
	@echo "  clean         - Remove all built examples"
	@echo "  help          - Show this help"
	@echo ""
	@echo "Examples (one ioctl concept each):"
	@echo "  discover      - QUERYCAP: scan devices, show capabilities"
	@echo "  formats       - ENUM_FMT / G/S/TRY_FMT / ENUM_FRAMESIZES"
	@echo "  controls      - QUERYCTRL / G_CTRL / S_CTRL / QUERYMENU"
	@echo "  buffers       - REQBUFS / QUERYBUF / mmap (no streaming)"
	@echo ""
	@echo "Usage:"
	@echo "  make all"
	@echo "  ./discover"
	@echo "  ./formats /dev/video0"
	@echo "  ./controls /dev/video0"
	@echo "  ./buffers /dev/video0"

.PHONY: all clean help
