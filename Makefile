# Crossnode build. Run inside a QNX SDP 8.0 environment (source qnxsdp-env.sh first).
#   make                 x86_64 binaries in build/x86_64
#   make ARCH=aarch64le  ARM binaries in build/aarch64le

ARCH   ?= x86_64
CC      = qcc -Vgcc_nto$(ARCH)
CFLAGS ?= -O2 -g -std=gnu11 -Wall -Wextra -Werror
OUT     = build/$(ARCH)
BINS    = $(OUT)/crossnode-net $(OUT)/crossnode-agent $(OUT)/hellodev $(OUT)/zonesensor $(OUT)/cnprobe $(OUT)/cnmitm \
          $(OUT)/crossnode-net-noverify

all: $(BINS)

$(OUT):
	mkdir -p $@

$(OUT)/crossnode-net: src/cn_net.c src/cn_proto.h src/cn_auth.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ src/cn_net.c -lsocket

$(OUT)/crossnode-agent: src/cn_agent.c src/cn_proto.h src/cn_auth.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ src/cn_agent.c -lsocket

$(OUT)/hellodev: tools/hellodev.c tools/hello_dcmd.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ tools/hellodev.c

$(OUT)/zonesensor: tools/zonesensor.c tools/hello_dcmd.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ tools/zonesensor.c

# Test-only builds: a man in the middle for the integrity checks, and a crossnode-net that skips the frame check
# (the mutation the integrity checks must catch).
$(OUT)/cnmitm: tools/cnmitm.c src/cn_proto.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ tools/cnmitm.c -lsocket

$(OUT)/crossnode-net-noverify: src/cn_net.c src/cn_proto.h src/cn_auth.h | $(OUT)
	$(CC) $(CFLAGS) -DCN_NO_VERIFY -o $@ src/cn_net.c -lsocket

$(OUT)/cnprobe: tools/cnprobe.c tools/hello_dcmd.h src/cn_proto.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ tools/cnprobe.c -lsocket

clean:
	rm -rf build

.PHONY: all clean
