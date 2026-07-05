# SPDX-License-Identifier: GPL-2.0
# StrayLight OS — XDP eBPF Programs
#
# Build BPF bytecode objects using clang.
# Requires: clang >= 12, linux-headers with BPF support.

CLANG       ?= clang
LLC         ?= llc
BPFTOOL     ?= bpftool
STRIP       ?= llvm-strip

# Target architecture
ARCH        ?= $(shell uname -m | sed 's/x86_64/x86/' | sed 's/aarch64/arm64/')

# Kernel and userspace header paths.  Debian multiarch keeps asm/*.h
# under /usr/include/<triplet>, which clang -target bpf will not infer.
KERN_INC    ?= /usr/src/linux-headers-$(shell uname -r)/include
LIBBPF_INC  ?= /usr/include/bpf
MULTIARCH_INC ?= $(shell triplet=$$(dpkg-architecture -qDEB_HOST_MULTIARCH 2>/dev/null); if [ -n "$$triplet" ] && [ -d "/usr/include/$$triplet" ]; then printf "/usr/include/%s" "$$triplet"; fi)

# BPF compiler flags
BPF_CFLAGS  := -target bpf \
               -D__TARGET_ARCH_$(ARCH) \
               -O2 \
               -g \
               -Wall \
               -Werror \
               -std=gnu11 \
               -I$(KERN_INC) \
               $(if $(MULTIARCH_INC),-I$(MULTIARCH_INC)) \
               -I$(LIBBPF_INC) \
               -I. \
               -D__BPF_TRACING__

# Output objects
BPF_PROGS   := xdp_filter.bpf.o xdp_redirect.bpf.o xdp_stats.bpf.o

.PHONY: all clean install

all: $(BPF_PROGS)

# Compile BPF C -> BPF ELF object
%.bpf.o: %.bpf.c xdp_maps.h
	$(CLANG) $(BPF_CFLAGS) -c $< -o $@
	@echo "  BPF     $@"

# Optional: generate BPF skeletons for userspace loaders
%.skel.h: %.bpf.o
	$(BPFTOOL) gen skeleton $< > $@
	@echo "  SKEL    $@"

# Strip debug info for production
%.stripped.o: %.bpf.o
	$(STRIP) -g $< -o $@
	@echo "  STRIP   $@"

# Verify BPF programs load correctly
verify: $(BPF_PROGS)
	@for prog in $(BPF_PROGS); do \
		echo "Verifying $$prog..."; \
		$(BPFTOOL) prog load $$prog /sys/fs/bpf/test_$$prog 2>/dev/null && \
			echo "  OK" && \
			rm -f /sys/fs/bpf/test_$$prog || \
			echo "  SKIP (need root)"; \
	done

# Install to the runtime path consumed by /etc/straylight/xdp.conf.
INSTALL_DIR ?= /usr/lib/straylight/bpf

install: $(BPF_PROGS)
	install -d $(DESTDIR)$(INSTALL_DIR)
	install -m 644 $(BPF_PROGS) $(DESTDIR)$(INSTALL_DIR)/
	@echo "Installed XDP programs to $(DESTDIR)$(INSTALL_DIR)"

clean:
	rm -f $(BPF_PROGS) *.skel.h *.stripped.o

# Show map info
info:
	@echo "XDP Programs:"
	@echo "  xdp_filter.bpf.o   - IP blocklist + rate limiting"
	@echo "  xdp_redirect.bpf.o - VLAN/IP-based packet redirect"
	@echo "  xdp_stats.bpf.o    - Per-CPU statistics collector"
	@echo ""
	@echo "Shared Maps:"
	@echo "  blocklist       - IPv4 blocklist (hash)"
	@echo "  blocklist_v6    - IPv6 blocklist (hash)"
	@echo "  redirect_map    - DEVMAP for redirect targets"
	@echo "  redirect_cfg    - Redirect configuration"
	@echo "  stats_map       - Per-action stats (percpu array)"
	@echo "  rate_limit_map  - Per-source rate counters (LRU hash)"
	@echo "  rate_limit_cfg  - Rate limit configuration"
