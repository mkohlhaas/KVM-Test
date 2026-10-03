# KVM-Test

A minimal sample demonstrating the Linux `/dev/kvm` API directly, with no
hypervisor framework or emulator underneath.

The program creates a virtual machine with a single vCPU, loads 13 bytes of
hand-assembled x86 machine code into it, and runs it. There is no guest OS —
just 4 KiB of guest RAM and a handful of registers. Serial I/O is served by
the hypervisor itself: each `out` instruction traps out to the host, which
prints the byte and resumes the guest.

`kvmtest.c` is commented step by step (steps 1–16), covering what each ioctl
does, why its arguments look the way they do, and what state the guest is put
into along the way.

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

**1. Guest memory allocation** — step 4, backing store for guest RAM

```diff
     mem = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
-    if (!mem)
+    if (mem == MAP_FAILED)
         err(1, "allocating guest memory");
```

A failure here would previously have crashed at the `memcpy` just below it.

**2. vCPU run-area mapping** — step 8, mapping the vCPU run area

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
Other than the fixes above and comments, the sample is unmodified.

## How it works

### KVM requests

The sample issues nine ioctls. Which descriptor a request is issued on
decides what it acts on, and there are only three kinds:

- `kvm` — the system device obtained from `open("/dev/kvm")`. System-wide
  requests only: API version, VM creation, run-area sizing.
- `vmfd` — one VM. Requests about that VM's memory map and its vCPUs.
- `vcpufd` — one vCPU. Requests about that vCPU's register state and its
  execution.

KVM's requests follow three conventions for passing data: some return a new
file descriptor, some return a plain value, and some take a pointer to a
structure the kernel either fills in or reads out.

Step numbering matches the step comments in `kvmtest.c`.

| Step | Request | On | Argument | Effect |
| --- | --- | --- | --- | --- |
| 1 | `open("/dev/kvm")` | — | `O_RDWR \| O_CLOEXEC` | Acquire the KVM system device. Not a KVM request. |
| 2 | `KVM_GET_API_VERSION` | `kvm` | `NULL` | Returns the API version; must be 12. |
| 3 | `KVM_CREATE_VM` | `kvm` | `0`, the VM type | Returns a new VM fd. |
| 4 | `mmap` | — | `GUEST_MEM_SIZE`, anonymous | Back one page of host memory with the guest program. Not a KVM request. |
| 5 | `KVM_SET_USER_MEMORY_REGION` | `vmfd` | `&region` | Register that page as guest RAM at physical `0x1000`. |
| 6 | `KVM_CREATE_VCPU` | `vmfd` | `0`, the vCPU index | Returns a vCPU fd. |
| 7 | `KVM_GET_VCPU_MMAP_SIZE` | `kvm` | `NULL` | Returns the run-area size, including space for per-exit data. |
| 8 | `mmap` on `vcpufd` | — | offset 0 | Map the shared `struct kvm_run` region used to report exit reasons. Not a KVM request. |
| 9 | `KVM_GET_SREGS` | `vcpufd` | `&sregs` | Read the special registers. First half of a read-modify-write. |
| 9 | `KVM_SET_SREGS` | `vcpufd` | `&sregs` | Write them back with `cs.base = 0` and `cs.selector = 0`, keeping the vCPU in real mode and bypassing paging and the real-mode interrupt descriptor table. |
| 10 | `KVM_SET_REGS` | `vcpufd` | `&regs` | Set `rip = 0x1000`, `rax = rbx = 2`, and `rflags = 0x2` (the architecturally reserved bit that must be set). |
| 11 | `KVM_RUN` | `vcpufd` | `NULL` | Enter the guest; returns on VM exit with `run->exit_reason` set. |

Steps 1, 4 and 8 are ordinary `open`/`mmap` calls rather than KVM requests.
Steps 12–16 are not requests either: they inspect what `KVM_RUN` reported and
are covered under [Exit handling](#exit-handling).

The two memory quantities are named in the code as `GUEST_MEM_SIZE` (the slot
length) and `GUEST_LOAD_ADDR` (the guest physical address it starts at),
currently both `0x1000`.

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
