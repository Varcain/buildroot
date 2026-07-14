#!/bin/sh
# oveRTOS rootfs post-build. $1 = target dir.
#
# 1. Provide the PT_INTERP path the FDPIC binaries name (/usr/lib/ld.so.1) as a symlink to
#    the real loader, so the personality can resolve the interpreter by its named path.
# 2. Strip the debug-info-heavy uClibc + libgcc_s + busybox (they ship unstripped — no
#    global BR2_STRIP): QSPI has room for the rootfs, but avoid wasting it. dbgdemo
#    (compiled -g in step 3, AFTER the strip) keeps its source-level-debug symbols.
# 3. Compile the personality's test programs (sources in board/overtos/progs) into /usr/bin,
#    so the QSPI rootfs.cpio carries them for CI + on-target debugging (t_pth and
#    t_pmin exercise LinuxThreads; dbgdemo is built -g for source-level debug; segv is the
#    negative KERNEL-isolation test — a deliberate kernel-SRAM write, denied by the ARM default
#    map; xregion is the negative INTER-PROGRAM-isolation test — a write to a SIBLING's pool
#    region, denied by the privileged-only whole-pool base MPU region (Phase 2). Both must fault
#    and be contained).
set -e
TARGET="$1"
if [ -e "$TARGET/lib/ld-uClibc.so.0" ]; then
	mkdir -p "$TARGET/usr/lib"
	ln -sf /lib/ld-uClibc.so.0 "$TARGET/usr/lib/ld.so.1"
fi

# curl verifies against the single CA bundle (built with --with-ca-bundle=
# /etc/ssl/certs/ca-certificates.crt). The stock ca-certificates GEN_BUNDLE
# finalize hook regenerates that bundle from $TARGET/usr/share/ca-certificates on
# EVERY build; we delete that dir + the ~430 per-CA /etc/ssl/certs symlinks below
# to stay under the personality's CPIO file-count limit (ROOTFS_MAX_FILES), but
# doing so leaves the finalize hook nothing to concatenate on the next
# incremental build → a 0-byte bundle → curl "certificate not signed by trusted
# CA". So rebuild the bundle here directly from the package's persistent build
# directory (untouched by the trim), then drop the redundant per-CA files.
BUILD_DIR="${BUILD_DIR:-$(dirname "$TARGET")/build}"
CERT_SRC="$(ls -d "$BUILD_DIR"/ca-certificates-*/mozilla 2>/dev/null | head -1)"
if [ -n "$CERT_SRC" ] && [ -d "$CERT_SRC" ]; then
	mkdir -p "$TARGET/etc/ssl/certs"
	# The full 144-root Mozilla bundle (~216 KB) overruns curl's per-process heap
	# on this NOMMU/FDPIC target — mbedtls buffers the whole file and parses every
	# cert, so verification silently ends up with no usable root ("certificate not
	# signed by trusted CA"). Ship a curated set of widely-deployed roots (~18 KB)
	# instead: Let's Encrypt (ISRG), DigiCert, USERTrust/Sectigo (Comodo), Google
	# Trust Services, Amazon, and SSL.com — the last is example.com's trust anchor.
	CURATED="SSL.com_TLS_ECC_Root_CA_2022 SSL.com_TLS_RSA_Root_CA_2022 \
		ISRG_Root_X1 ISRG_Root_X2 \
		DigiCert_Global_Root_CA DigiCert_Global_Root_G2 DigiCert_Global_Root_G3 \
		USERTrust_ECC_Certification_Authority USERTrust_RSA_Certification_Authority \
		Sectigo_Public_Server_Authentication_Root_E46 Sectigo_Public_Server_Authentication_Root_R46 \
		GTS_Root_R1 GTS_Root_R4 Amazon_Root_CA_1"
	: > "$TARGET/etc/ssl/certs/ca-certificates.crt"
	for r in $CURATED; do
		[ -f "$CERT_SRC/$r.crt" ] && cat "$CERT_SRC/$r.crt" >> "$TARGET/etc/ssl/certs/ca-certificates.crt"
	done
	find "$TARGET/etc/ssl/certs" -mindepth 1 ! -name ca-certificates.crt -delete
	rm -rf "$TARGET/usr/share/ca-certificates"
fi

STRIP="$HOST_DIR/bin/arm-buildroot-uclinuxfdpiceabi-strip"
if [ -x "$STRIP" ]; then
	"$STRIP" --strip-unneeded "$TARGET"/lib/libuClibc-*.so "$TARGET"/lib/libgcc_s.so.1 "$TARGET"/bin/busybox 2>/dev/null || true
fi

GCC="$HOST_DIR/bin/arm-buildroot-uclinuxfdpiceabi-gcc"
PROGS="$(CDPATH= cd -- "$(dirname -- "$0")/progs" 2>/dev/null && pwd)"
if [ -x "$GCC" ] && [ -n "$PROGS" ]; then
	mkdir -p "$TARGET/usr/bin"
	"$GCC" -mfdpic -O2 "$PROGS/t_pth.c" -o "$TARGET/usr/bin/t_pth" -pthread
	"$GCC" -mfdpic -O2 "$PROGS/t_pmin.c" -o "$TARGET/usr/bin/t_pmin" -pthread
	"$GCC" -mfdpic -g -O0 "$PROGS/dbgdemo.c" -o "$TARGET/usr/bin/dbgdemo"
	"$GCC" -mfdpic -O2 "$PROGS/segv.c" -o "$TARGET/usr/bin/segv"
	"$GCC" -mfdpic -O2 "$PROGS/xregion.c" -o "$TARGET/usr/bin/xregion"
	"$GCC" -mfdpic -O2 "$PROGS/kstress.c" -o "$TARGET/usr/bin/kstress"
	"$GCC" -mfdpic -O2 "$PROGS/lbench.c" -o "$TARGET/usr/bin/lbench"
	"$GCC" -mfdpic -O2 "$PROGS/fbtest.c" -o "$TARGET/usr/bin/fbtest"
	"$GCC" -mfdpic -O2 "$PROGS/evread.c" -o "$TARGET/usr/bin/evread"
	"$GCC" -mfdpic -O2 "$PROGS/nettest.c" -o "$TARGET/usr/bin/nettest"
	# tlsprobe: mbedTLS crypto self-test (AES/GCM/SHA vectors) — needs the staged mbedtls.
	if [ -f "$STAGING_DIR/usr/lib/libmbedcrypto.so" ] && [ -f "$PROGS/tlsprobe.c" ]; then
		"$GCC" -mfdpic -O2 "$PROGS/tlsprobe.c" -o "$TARGET/usr/bin/tlsprobe" \
			-I"$STAGING_DIR/usr/include" -L"$STAGING_DIR/usr/lib" \
			-lmbedx509 -lmbedcrypto
	fi
	if [ -f "$STAGING_DIR/usr/lib/libmbedcrypto.so" ] && [ -f "$PROGS/tlsclient.c" ]; then
		# tlsclient: minimal mbedTLS TLS client with debug tracing, to pinpoint
		# where the handshake fails after cert verification (curl (56)).
		"$GCC" -mfdpic -O2 "$PROGS/tlsclient.c" -o "$TARGET/usr/bin/tlsclient" \
			-I"$STAGING_DIR/usr/include" -L"$STAGING_DIR/usr/lib" \
			-lmbedtls -lmbedx509 -lmbedcrypto
	fi
fi
