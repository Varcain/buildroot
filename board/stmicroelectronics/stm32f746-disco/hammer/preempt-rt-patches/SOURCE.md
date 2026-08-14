# Linux 5.15.211-rt97 source

`linux/0001-linux-5.15.211-rt97.patch` is the uncompressed official
PREEMPT_RT patch published at:

https://cdn.kernel.org/pub/linux/kernel/projects/rt/5.15/patch-5.15.211-rt97.patch.gz

The downloaded gzip member matched the kernel.org `sha256sums.asc` entry:

```
8b4ce41c930c158792cfe5094d01372573593587a6885b148a9d92c99ab1c3e2  patch-5.15.211-rt97.patch.gz
```

The uncompressed patch committed here has SHA-256:

```
da029007eaafed7196babc2820dab18017b95fcb88349f79c58d1eef3dc32c67  0001-linux-5.15.211-rt97.patch
```

This patch directory is deliberately included only by the PREEMPT_RT
Buildroot defconfig. It is ordered before the board's local Linux patches so
the latter continue to apply against the RT source as an independently
testable variant.
