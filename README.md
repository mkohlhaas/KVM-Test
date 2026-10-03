# KVM-Test

A minimal sample demonstrating the Linux `/dev/kvm` API directly, with no
hypervisor framework or emulator underneath.

The program creates a virtual machine with a single vCPU, loads 13 bytes of
hand-assembled x86 machine code into it, and runs it. There is no guest OS —
just 4 KiB of guest RAM and a handful of registers. Serial I/O is served by
the hypervisor itself: each `out` instruction traps out to the host, which
prints the byte and resumes the guest.

[Upstream sample](https://lwn.net/Articles/658511/): Copyright (c) 2015 Intel Corporation, authored by
Josh Triplett &lt;josh@joshtriplett.org&gt;. Distributed under the MIT license
(see the header of `kvmtest.c`).

## Building

Requires a C compiler, the KVM UAPI headers (`linux/kvm.h`), and glibc's BSD
error functions (`err.h`, which lives in glibc, not libbsd).

```sh
gcc -Wall -Wextra -o kvmtest kvmtest.c
```

Or use the included Makefile, which builds the same binary:

```sh
make            # build kvmtest
make run        # build, then run it
make clean      # remove the binary
```

`CC` and `CFLAGS` can be overridden, for example `make CC=clang` or
`make CFLAGS='-O2 -Wall -Wextra'`. Note that `CC` defaults to make's built-in
`cc`, which on most distributions is a symlink to gcc.

For an optimized build without the Makefile:

```sh
gcc -O2 -Wall -Wextra -o kvmtest kvmtest.c
```

## Running

```sh
./kvmtest
```

Expected output:

```
4
KVM_EXIT_HLT
```

### Requirements

The host must expose `/dev/kvm`, which requires a CPU with hardware
virtualization support and the `kvm` kernel modules loaded:

```sh
sudo modprobe kvm
sudo modprobe kvm_intel     # or kvm_amd, depending on your CPU
```

You need permission to open `/dev/kvm`. It is owned by `root:kvm` and mode
`0666` on most distributions, so membership of the `kvm` group is sufficient:

```sh
sudo usermod -aG kvm $USER   # then log out and back in
```

If access is denied, run under `sudo` instead.

## Changes made to the upstream sample

Two latent error-handling bugs were fixed. Both involved checking the result
of `mmap` against `NULL`, but `mmap` reports failure by returning
`MAP_FAILED` (`(void *)-1`) and never returns `NULL`. The checks therefore
could never fire: on failure, execution fell through and dereferenced an
invalid pointer, producing a segfault instead of the intended diagnostic.

**1. Guest memory allocation** — `kvmtest.c:70`

```diff
     mem = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
-    if (!mem)
+    if (mem == MAP_FAILED)
         err(1, "allocating guest memory");
```

A failure here would previously have crashed at the `memcpy` on line 72.

**2. vCPU run-area mapping** — `kvmtest.c:97`

```diff
     run = mmap(NULL, mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED, vcpufd, 0);
-    if (!run)
+    if (run == MAP_FAILED)
         err(1, "mmap vcpu");
```

A failure here would previously have crashed at the `run->exit_reason`
access in the run loop.

These are the only changes to the sample's logic. Because both mappings
succeed on a normally configured host, observable behavior is unchanged.

## How it works

| Step | Purpose |
| --- | --- |
| `open("/dev/kvm")` | Acquire the KVM system device. |
| `KVM_GET_API_VERSION` | Assert the stable API version (12). |
| `KVM_CREATE_VM` | Create a VM. |
| `mmap` + `KVM_SET_USER_MEMORY_REGION` | Register one page of guest RAM at physical `0x1000` and copy in the code. |
| `KVM_CREATE_VCPU` | Create one vCPU. |
| `KVM_GET_VCPU_MMAP_SIZE` + `mmap` | Map the shared `struct kvm_run` region used to report exit reasons. |
| `KVM_GET_SREGS` / `KVM_SET_SREGS` | Force `cs.base = 0` and `cs.selector = 0` so the CPU stays in real mode, bypassing paging and the real-mode interrupt descriptor table. |
| `KVM_SET_REGS` | Set `rip = 0x1000`, `rax = rbx = 2`, and `rflags = 0x2` (the architecturally reserved bit that must be set). |
| `KVM_RUN` loop | Run the guest and dispatch on `run->exit_reason`. |

### The guest program

Placed at guest physical address `0x1000`, in 16-bit real mode:

```asm
mov $0x3f8, %dx   ; DX = COM1 serial port
add %bl, %al      ; AL = 2 + 2 = 4          (rax = 2, rbx = 2)
add $'0', %al     ; AL = 4 + 0x30 = '4'
out %al, (%dx)    ; VM exit; host prints '4'
mov $'\n', %al
out %al, (%dx)    ; VM exit; host prints newline
hlt
```

### Exit handling

- **`KVM_EXIT_IO`** — validated as a single-byte `OUT` to port `0x3f8`, then
  the byte is read from the vCPU mapping at `run->io.data_offset` and passed
  to `putchar`.
- **`KVM_EXIT_HLT`** — the guest has finished; prints `KVM_EXIT_HLT` and
  returns success.
- **`KVM_EXIT_FAIL_ENTRY`**, **`KVM_EXIT_INTERNAL_ERROR`**, and any other
  reason — treated as fatal, with the reason reported.
