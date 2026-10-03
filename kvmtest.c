/* Sample code for /dev/kvm API
 *
 * Copyright (c) 2015 Intel Corporation
 * Author: Josh Triplett <josh@joshtriplett.org>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <err.h>
#include <fcntl.h>
#include <linux/kvm.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

/* Size of the guest memory slot: one x86 page, 4 KiB. KVM requires both the
 * start address and the length of a memory slot to be page-aligned. */
#define GUEST_MEM_SIZE 0x1000

/* Guest physical address the slot starts at: one page in, so the page at
 * physical 0 -- where the real-mode interrupt descriptor table lives -- is
 * left unmapped. Spelled in terms of GUEST_MEM_SIZE because the two only
 * coincide by construction: this guest is one page, placed in the second. */
#define GUEST_LOAD_ADDR GUEST_MEM_SIZE

/* Trace a step of the host program to stderr.
 *
 * stderr rather than stdout so that the guest's serial bytes, which go to
 * stdout through putchar(), stay the program's only stdout output. The
 * interleaving of the two streams is what makes the control flow readable.
 */
#define trace(...) fprintf(stderr, "kvmtest: " __VA_ARGS__)

/* The guest program: 13 bytes of hand-assembled x86 machine code.
 *
 * It is written for 16-bit real mode, where instructions are encoded without
 * the REX prefixes and operand sizes default to 16 bits, which keeps the
 * encodings short. Every byte below is already encoded, so nothing has to be
 * assembled at runtime.
 *
 * Trace of one run (rbx = 2, rax = 2 on entry):
 *   mov $0x3f8, %dx   DX = 0x3f8, the COM1 serial port
 *   add %bl, %al      AL = 2 + 2 = 4
 *   add $'0', %al     AL = 4 + 0x30 = '4'
 *   out %al, (%dx)    traps out of the guest; the host prints '4'
 *   mov $'\n', %al
 *   out %al, (%dx)    traps out of the guest; the host prints a newline
 *   hlt               traps out of the guest; the host treats this as success
 *
 * There is no device model behind port 0x3f8: KVM reports the port I/O and
 * lets the host perform it, which is exactly what the exit handler below does.
 */
const uint8_t code[] = {
    0xba, 0xf8, 0x03, /* mov $0x3f8, %dx */
    0x00, 0xd8,       /* add %bl, %al */
    0x04, '0',        /* add $'0', %al */
    0xee,             /* out %al, (%dx) */
    0xb0, '\n',       /* mov $'\n', %al */
    0xee,             /* out %al, (%dx) */
    0xf4,             /* hlt */
};

int main(void) {
  /* stdout is line-buffered on a terminal but block-buffered when redirected
   * to a file, which would let the stderr trace and the guest's bytes arrive
   * out of order in a log. Line-buffering unconditionally keeps the two
   * streams in step wherever they end up. */
  setvbuf(stdout, NULL, _IOLBF, 0);

  /* Throughout main, ioctl() returns -1 with errno set on failure, so every
   * call is checked for that. The error helpers exit immediately and never
   * return:
   *   err(1, msg)  -- print "msg: strerror(errno)" and exit with status 1
   *   errx(1, msg) -- print "msg" (no errno part) and exit with status 1
   */

  /* Step 1: open the KVM system device.
   *
   * This is the top-level fd, used only for system-wide ioctls: querying the
   * API version, creating VMs, and asking how large a vCPU run area is.
   * It is not tied to any particular VM.
   *
   *   O_RDWR    required; VM and vCPU operations only work on a writable fd
   *   O_CLOEXEC close the fd across any later exec() instead of leaking it
   */
  int kvm = open("/dev/kvm", O_RDWR | O_CLOEXEC);
  if (kvm == -1)
    err(1, "/dev/kvm");
  trace("step 1:  /dev/kvm opened as fd %d\n", kvm);

  /* Step 2: verify the KVM API version.
   *
   * KVM_GET_API_VERSION reports the API the kernel implements. KVM_API_VERSION
   * (12) is the stable API: the ioctl numbers and the layout of the structures
   * passed to them are fixed for it. A different value means either an older
   * kernel or an out-of-tree module with a different ABI, so continuing could
   * mean passing structures the kernel would misinterpret -- bail out instead.
   */
  int ret = ioctl(kvm, KVM_GET_API_VERSION, NULL);
  if (ret == -1)
    err(1, "KVM_GET_API_VERSION");
  if (ret != 12)
    errx(1, "KVM_GET_API_VERSION %d, expected 12", ret);
  trace("step 2:  KVM API version %d\n", ret);

  /* Step 3: create a virtual machine.
   *
   * KVM_CREATE_VM returns a VM fd, which owns the guest's memory map, its
   * vCPUs, and its overall lifecycle. The argument is the VM type; 0 is the
   * only type defined so far, hence the (unsigned long)0 cast to match the
   * untyped variadic ioctl() signature.
   */
  int vmfd = ioctl(kvm, KVM_CREATE_VM, (unsigned long)0);
  if (vmfd == -1)
    err(1, "KVM_CREATE_VM");
  trace("step 3:  VM created as fd %d\n", vmfd);

  /* Step 4: allocate backing store for guest RAM.
   *
   * Guest memory is ordinary host memory supplied by userspace, so KVM never
   * allocates RAM itself. Here GUEST_MEM_SIZE bytes are mapped anonymously:
   *
   *   NULL              let the kernel choose the host address
   *   GUEST_MEM_SIZE    one page, the length of the memory slot below
   *   PROT_READ|WRITE   the guest may both read and write this page
   *   MAP_SHARED        real memory, not a private copy; the guest's writes
   *                     must be visible to the host
   *   MAP_ANONYMOUS     no file backing; the pages start out zeroed
   *   fd -1             required for anonymous mappings
   */
  uint8_t *mem = mmap(NULL, GUEST_MEM_SIZE, PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED)
    err(1, "allocating guest memory");

  /* Copy the guest program to the start of the page. The remainder of the
   * page stays zero, which is harmless: nothing ever executes it. */
  memcpy(mem, code, sizeof(code));
  trace("step 4:  %zu bytes of guest RAM backed at host address %p\n",
        (size_t)GUEST_MEM_SIZE, (void *)mem);

  /* Step 5: register that page as guest physical memory.
   *
   * A struct kvm_userspace_memory_region describes one slot in the guest's
   * physical address space. It is passed by pointer and copied in by the
   * kernel, so a stack temporary is enough.
   */
  struct kvm_userspace_memory_region region = {
      /* Slots are keyed by number; a one-page guest needs only one. */
      .slot = 0,
      /* Guest physical address where this slot starts, i.e. GUEST_LOAD_ADDR.
       * It is deliberately not zero: with cs.base = 0 below, the linear
       * address the vCPU fetches from equals the guest physical address, so
       * leaving the first page unmapped keeps execution clear of the
       * real-mode interrupt descriptor table that lives at physical 0. */
      .guest_phys_addr = GUEST_LOAD_ADDR,
      /* Length of the slot; matches the mapping length used in step 4. */
      .memory_size = GUEST_MEM_SIZE,
      /* Host address of the mapping registered by this slot. */
      .userspace_addr = (uint64_t)mem,
  };
  ret = ioctl(vmfd, KVM_SET_USER_MEMORY_REGION, &region);
  if (ret == -1)
    err(1, "KVM_SET_USER_MEMORY_REGION");
  trace("step 5:  slot %u registered at guest physical %#x, %zu bytes\n",
        region.slot, (unsigned int)GUEST_LOAD_ADDR, (size_t)GUEST_MEM_SIZE);

  /* Step 6: create a virtual CPU.
   *
   * KVM_CREATE_VCPU returns a vCPU fd, on which the run area can be mapped
   * and which KVM_RUN is later issued against. The argument is the vCPU
   * index and must be 0, since the sample asks for one vCPU.
   */
  int vcpufd = ioctl(vmfd, KVM_CREATE_VCPU, (unsigned long)0);
  if (vcpufd == -1)
    err(1, "KVM_CREATE_VCPU");
  trace("step 6:  vCPU created as fd %d\n", vcpufd);

  /* Step 7: ask how large the vCPU run area needs to be.
   *
   * KVM_GET_VCPU_MMAP_SIZE returns the size of the shared region that reports
   * what happened while the guest was running. It covers struct kvm_run plus
   * room for per-exit data (such as the bytes of a port I/O operation), whose
   * offset is expressed relative to the start of that region.
   *
   * The size must be queried rather than assumed: the kernel may add new exit
   * data over time, so hardcoding sizeof(struct kvm_run) can under-map it.
   */
  ret = ioctl(kvm, KVM_GET_VCPU_MMAP_SIZE, NULL);
  if (ret == -1)
    err(1, "KVM_GET_VCPU_MMAP_SIZE");

  /* The ioctl returned the size in its return value, since it passes no
   * output argument. */
  size_t mmap_size = ret;

  /* Step 8: map the vCPU run area.
   *
   * Refuse to map a region too small to hold struct kvm_run; the code below
   * dereferences it, and the kernel should never report such a size. */
  struct kvm_run *run;
  if (mmap_size < sizeof(*run))
    errx(1, "KVM_GET_VCPU_MMAP_SIZE unexpectedly small");

  /* vcpufd is a KVM-typed fd: offset 0 of this mapping is the run area.
   * MAP_SHARED is what makes it live -- the kernel writes exit details in
   * after KVM_RUN, and reads certain fields out of it (for example
   * request_interrupt_window) to configure the next run. */
  run = mmap(NULL, mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED, vcpufd, 0);
  if (run == MAP_FAILED)
    err(1, "mmap vcpu");
  trace("step 7:  run area is %zu bytes\n", mmap_size);
  trace("step 8:  run area mapped at host address %p\n", (void *)run);

  /* Step 9: force the vCPU into 16-bit real mode.
   *
   * Special registers (segment registers, CR0, EFER, ...) are read as a whole
   * and written back as a whole, so the change is a read-modify-write: read
   * the current state, adjust only what is needed, write the whole thing
   * back. Writing a partially filled struct would zero every field left out.
   */
  struct kvm_sregs sregs;
  ret = ioctl(vcpufd, KVM_GET_SREGS, &sregs);
  if (ret == -1)
    err(1, "KVM_GET_SREGS");

  /* Zero code base: with a zero CS base, linear addresses equal physical
   * addresses, so the guest executes the page in place with no translation
   * and without paging being involved. */
  sregs.cs.base = 0;
  /* Zero code selector: the reset state selects flat real-mode segments, and
   * 0 is the only value a selector may hold while in real mode. */
  sregs.cs.selector = 0;

  ret = ioctl(vcpufd, KVM_SET_SREGS, &sregs);
  if (ret == -1)
    err(1, "KVM_SET_SREGS");
  trace("step 9:  cs.base and cs.selector cleared, vCPU left in real mode\n");

  /* Step 10: set the general-purpose registers and the entry point.
   *
   * These are set all at once via KVM_SET_REGS; there is no read-modify-write
   * needed, since every field written below covers the whole struct.
   */
  struct kvm_regs regs = {
      /* Instruction pointer: where execution begins. Matches the start of
       * the guest program, which the memory slot places at GUEST_LOAD_ADDR. */
      .rip = GUEST_LOAD_ADDR,
      /* Guest code computes 2 + 2 = 4 and converts that to ASCII. */
      .rax = 2,
      .rbx = 2,
      /* Flags. Bit 1 is reserved by the architecture and must read as 1 or
       * the guest is rejected with KVM_EXIT_FAIL_ENTRY; everything else is
       * cleared, so in particular interrupts are masked. */
      .rflags = 0x2,
  };
  ret = ioctl(vcpufd, KVM_SET_REGS, &regs);
  if (ret == -1)
    err(1, "KVM_SET_REGS");
  trace("step 10: rip = %#x, rax = rbx = 2, rflags = 0x2\n",
        (unsigned int)GUEST_LOAD_ADDR);

  /* Step 11: run the guest and handle what it does on the way out.
   *
   * KVM_RUN enters the guest and blocks in the CPU until the vCPU exits back
   * to the host, then returns 0. The reason for the exit is left in the shared
   * run area as run->exit_reason, and the accompanying details in the union
   * member that goes with it.
   *
   * (KVM_RUN also returns -1/EINTR if a signal arrives before the guest is
   * entered. This sample has no signal handlers and treats any -1 as fatal.)
   */
  while (1) {
    trace("step 11: KVM_RUN, entering the guest\n");
    ret = ioctl(vcpufd, KVM_RUN, NULL);
    if (ret == -1)
      err(1, "KVM_RUN");

    switch (run->exit_reason) {
    /* Step 12: the guest executed HLT, so it has finished its work. Print a
     * marker and exit successfully. */
    case KVM_EXIT_HLT:
      trace("         guest exited: KVM_EXIT_HLT, it halted\n");
      puts("KVM_EXIT_HLT");
      return 0;

    /* Step 13: the guest performed port I/O. This sample serves it from the
     * host instead of emulating a serial device in hardware. */
    case KVM_EXIT_IO:
      /* Handle exactly the one case it knows about: a single-byte OUT to
       * COM1. Anything else -- a different port, a wider transfer, several
       * bytes at once, or an IN rather than an OUT -- is a guest request the
       * host has no answer for, so refuse it instead of guessing. */
      if (run->io.direction == KVM_EXIT_IO_OUT && run->io.size == 1 &&
          run->io.port == 0x3f8 && run->io.count == 1) {
        /* The transferred bytes are not in the struct; data_offset locates
         * them within the run mapping, relative to its start. Since size is
         * 1, there is a single byte, which is the character to print. */
        unsigned char byte = *(((char *)run) + run->io.data_offset);
        trace("         guest exited: KVM_EXIT_IO, %u byte out to port %#x"
              ", resuming the guest\n",
              (unsigned int)run->io.size, (unsigned int)run->io.port);
        putchar(byte);
      } else {
        errx(1, "unhandled KVM_EXIT_IO");
      }
      break;

    /* Step 14: the hardware refused to enter the guest, which usually means
     * the register state loaded in step 10 is invalid. The reason is
     * architecture-specific, so print it in hex and stop. */
    case KVM_EXIT_FAIL_ENTRY:
      errx(1, "KVM_EXIT_FAIL_ENTRY: hardware_entry_failure_reason = 0x%llx",
           (unsigned long long)run->fail_entry.hardware_entry_failure_reason);

    /* Step 15: KVM itself hit an error while running the guest. suberror
     * gives the category of failure. */
    case KVM_EXIT_INTERNAL_ERROR:
      errx(1, "KVM_EXIT_INTERNAL_ERROR: suberror = 0x%x",
           run->internal.suberror);

    /* Step 16: any other exit reason is unhandled. Log the raw value rather
     * than assuming a meaning for it. */
    default:
      errx(1, "exit_reason = 0x%x", run->exit_reason);
    }
  }
}
