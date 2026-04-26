# GpuMMIOFix — UEFI Driver to Fix GPU MMIO / PCI BAR Remapping Above the 4 GB Limit

GpuMMIOFix is a vendor-agnostic UEFI (EFI) driver that automatically relocates GPU PCI BAR (Base Address Register) resources into the 32-bit MMIO window (below 4 GB) before the operating system boots. It solves the well-known problem where firmware does not properly map GPU MMIO regions when Above 4G Decoding is disabled, misconfigured or unavailable, causing GPUs to be invisible or broken in the OS, in hypervisors, or during PCI passthrough (VFIO/KVM).

## Problem: GPU MMIO Above 4 GB and the PCI BAR Remapping Issue

Modern discrete GPUs require large MMIO windows — often 256 MB to 1 GB — for their BARs (framebuffer, control registers, etc.). When the BIOS/UEFI places these BARs above the 4 GB physical address boundary (Above 4G Decoding), several issues can arise:
* Legacy OS or hypervisors that do not support 64-bit PCI BARs fail to enumerate the GPU.
* PCI passthrough (VFIO/KVM/QEMU) setups require resizable BARs to be within a 32-bit-addressable window for certain guest configurations.
* Firmware bugs on some motherboards leave GPU BARs uninitialized or conflicting.
* Thunderbolt / external GPU (eGPU) docks behind PCIe bridges can be left with unrouted MMIO windows.

GpuMMIOFix runs as a UEFI driver at the `ReadyToBoot` event, scans all PCI display-class devices (class code `0x03`), finds free MMIO space in the configured 32-bit range, and reprograms both the GPU BARs and the upstream PCIe bridge windows to route traffic correctly.

## Features
* **Vendor-agnostic** — works with NVIDIA, AMD, Intel, and any other display-class PCI device.
* **Multi-GPU support** — handles up to 8 GPUs simultaneously.
* **Prefetchable and non-prefetchable BAR separation** — allocates prefetchable BARs in a dedicated chunk (512 MB/GPU target, 256 MB fallback) and non-prefetchable BARs in a separate pool.
* **PCIe bridge window patching** — walks the full bridge chain upstream of each GPU and expands MMIO/prefetchable windows as needed.
* **Write-verify & Safe rollback** — after programming each BAR, the driver reads the value back. If verification fails or any GPU BAR fails to program, all previously programmed GPUs are restored to their original values.
* **Minimal footprint** — single C source file, no runtime allocations beyond the EFI boot-services pool.

## How It Works
1. **Discovery** — at the `ReadyToBoot` EFI event, the driver locates all PCI handles via `EFI_PCI_IO_PROTOCOL`.
2. **Bridge collection** — all PCI-to-PCI bridges are enumerated and their primary/secondary/subordinate bus numbers are recorded.
3. **Used MMIO collection** — all existing 32-bit BAR allocations are recorded to build a sorted map of occupied MMIO ranges.
4. **GPU detection** — PCI devices with class code `0x03` (Display) that sit behind at least one PCIe bridge are selected for relocation.
5. **MMIO planning** — `FindFreeBlock` walks the sorted used-ranges map to find contiguous free holes for both prefetchable and non-prefetchable pools. Auto-fallback to legacy mode triggers if the standard window is too fragmented.
6. **Bridge window patching** — `PatchBridgeWindows` expands the bridge's Memory Base/Limit and Prefetchable Memory Base/Limit registers to cover the new allocations. Write-verify is performed; bridges that ignore writes are rolled back immediately.
7. **BAR programming** — `WriteBarVerify` writes new base addresses into each BAR register and reads them back to confirm acceptance.

## Building
GpuMMIOFix is built with [GNU-EFI](https://sourceforge.net/projects/gnu-efi/). A standard `Makefile` is provided.

### Prerequisites
```bash
# Debian / Ubuntu
sudo apt install gnu-efi build-essential

# Fedora / RHEL
sudo dnf install gnu-efi-devel gcc make
```

### Compile
```bash
make
```
*Manual build (if needed):*
```bash
gcc -I/usr/include/efi -I/usr/include/efi/x86_64 \
    -fpic -ffreestanding -fno-stack-protector -fno-stack-check \
    -fshort-wchar -mno-red-zone -Wall \
    -DEFI_FUNCTION_WRAPPER \
    -c GpuMMIOFix.c -o GpuMMIOFix.o

ld.bfd -nostdlib -znocombreloc \
   -T /usr/lib/elf_x86_64_efi.lds \
   -shared -Bsymbolic \
   /usr/lib/crt0-efi-x86_64.o GpuMMIOFix.o \
   -o GpuMMIOFix.so \
   -L/usr/lib -lefi -lgnuefi

objcopy --input-target elf64-x86-64 --output-target efi-app-x86_64 \
        -j .text -j .sdata -j .data -j .dynamic \
        -j .dynsym -j .reloc GpuMMIOFix.so GpuMMIOFix.efi
```
The result is `GpuMMIOFix.efi`.

### Compile-time Flags
Pass via `CFLAGS` or `make CFLAGS="..."`:
* `-D USE_LEGACY_PCIIO_PATH` — use `<protocol/pciio.h>` instead of `<efi/efipciio.h>` for older/distro-specific gnu-efi layouts.
* `-D GNUEFI_VERSION=0x040000` — enable fallbacks for older gnu-efi versions.
* `-D FORCE_LEGACY_MMIO` — force legacy MMIO window mode (starts search at `0xC0000000`, disables prefetchable allocation).

## Installation
Copy `GpuMMIOFix.efi` to your EFI System Partition, for example:
`EFI\GpuMMIOFix\GpuMMIOFix.efi`

Register it as a UEFI driver in your firmware's boot manager:
**UEFI Shell (`startup.nsh`):**
```text
load fs0:\EFI\GpuMMIOFix\GpuMMIOFix.efi
```
**efibootmgr (Linux):**
```bash
efibootmgr --create --disk /dev/sda --part 1 \
  --label "GpuMMIOFix" \
  --loader '\EFI\GpuMMIOFix\GpuMMIOFix.efi'
```

Reboot. The driver runs automatically at `ReadyToBoot` (just before OS handoff) and prints progress to the EFI console.
*(Note: The current build includes an optional chainload hook for `\EFI\limine\limine_x64.efi`. Remove or guard the `LoadImage/StartImage` block in `efi_main` if unused.)*

## Configuration
All tuning constants are `#define`s at the top of `GpuMMIOFix.c`:

| Constant | Default | Description |
|---|---|---|
| `MMIO32_START` | `0xA0000000` | Start of the 32-bit MMIO search window (2.5 GB) |
| `MMIO32_END` | `0xFEC00000` | End of search window (avoids APIC/HPET regions) |
| `MAIN_ALIGN` | 1 MB | Minimum BAR alignment |
| `PF_PER_GPU_TARGET` | 512 MB | Preferred prefetchable allocation per GPU |
| `PF_PER_GPU_FALLBACK` | 256 MB | Fallback prefetchable allocation if target doesn't fit |
| `NP_SLACK_PER_GPU` | 8 MB | Extra non-prefetchable slack per GPU |
| `NP_MIN_TOTAL` | 64 MB | Minimum total non-prefetchable pool size |
| `MAX_GPU` | 8 | Maximum number of GPUs handled |

## Limitations
* Only targets PCI devices with class code `0x03` (Display / GPU). Other device types are not remapped.
* Only GPUs behind at least one PCIe bridge are relocated (root-complex-attached GPUs are skipped).
* The search window is limited to `[MMIO32_START, MMIO32_END)`. Systems with very crowded 32-bit MMIO maps may not have enough free space.
* Tested on x86-64 UEFI firmware. Other architectures are untested.
* Does not persist changes across reboots (runs at every boot via the UEFI driver load).

## License
This project is released under the GNU General Public License v2 (GPL-2.0), consistent with the GNU-EFI library it depends on. See [LICENSE](LICENSE) for details.
