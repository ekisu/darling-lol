#!/bin/sh
# Host check: can a KVM guest be made to answer CPUID like the emulated Mac CPU?
#
# Boots a 512-byte serial probe under QEMU/KVM twice: once with the host CPU and once
# with the darling-lol mask (GenuineIntel, family 6, model 158, stepping 10, the
# i7-9750H brand string, Intel-shaped leaf limits). Prints what the guest's `cpuid`
# instruction actually returns. Requires qemu-system-x86_64 with KVM, binutils.
#
# Usage: tools/kvm-cpuid-probe.sh [--cpu-mask "host,vendor=...,..."]
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=${TMPDIR:-/tmp}/kvm-cpuid-probe.$$
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

MASK=${1:-}
if [ "$MASK" = "--cpu-mask" ]; then
	MASK=${2:?missing cpu model}
else
	MASK="host,vendor=GenuineIntel,family=6,model=158,stepping=10,level=0x16,xlevel=0x80000008,model-id=Intel(R) Core(TM) i7-9750H CPU @ 2.60GHz"
fi

cat > "$work/probe.S" <<'ASM'
.code16
.section .text
.globl _start
_start:
    cli
    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw $0x7c00, %sp
    sti
    movw $0x3f9, %dx
    movb $0x00, %al
    outb %al, %dx
    movw $0x3fb, %dx
    movb $0x80, %al
    outb %al, %dx
    movw $0x3f8, %dx
    movb $0x01, %al
    outb %al, %dx
    movw $0x3f9, %dx
    xorw %ax, %ax
    outb %al, %dx
    movw $0x3fb, %dx
    movb $0x03, %al
    outb %al, %dx
    movw $0x3fa, %dx
    movb $0xc7, %al
    outb %al, %dx
    movw $0x3fc, %dx
    movb $0x0b, %al
    outb %al, %dx
    movl $0, %eax
    cpuid
    movl %ebx, vendor+0
    movl %edx, vendor+4
    movl %ecx, vendor+8
    movw $vendor, %si
    movw $12, %cx
    call print_n
    movw $nl, %si
    movw $1, %cx
    call print_n
    movl $0x80000000, %eax
    cpuid
    cmpl $0x80000004, %eax
    jb no_brand
    movw $brand, %di
    movl $0x80000002, %ebp
1:  movl %ebp, %eax
    cpuid
    movl %eax, (%di)
    movl %ebx, 4(%di)
    movl %ecx, 8(%di)
    movl %edx, 12(%di)
    addw $16, %di
    incl %ebp
    cmpl $0x80000004, %ebp
    jbe 1b
    movw $brand, %si
    movw $48, %cx
    call print_n
no_brand:
    movw $nl, %si
    movw $1, %cx
    call print_n
    movl $1, %eax
    cpuid
    call print_eax
    movl $0, %eax
    cpuid
    call print_eax
    movl $0x80000000, %eax
    cpuid
    call print_eax
hang:
    hlt
    jmp hang
print_n:
    lodsb
    movw $0x3f8, %dx
    outb %al, %dx
    loop print_n
    ret
print_eax:
    movl %eax, %ebx
    movw $8, %cx
5:  roll $4, %ebx
    movl %ebx, %eax
    andl $0xf, %eax
    cmpb $10, %al
    jb 6f
    addb $('a'-10), %al
    jmp 7f
6:  addb $'0', %al
7:  movw $0x3f8, %dx
    outb %al, %dx
    loop 5b
    movw $nl, %si
    movw $1, %cx
    call print_n
    ret
nl: .byte 13, 10
vendor: .space 12, 0
brand:  .space 48, 0
.org 510
.byte 0x55, 0xaa
ASM

as --32 -o "$work/probe.o" "$work/probe.S"
ld -m elf_i386 -Ttext 0x7c00 --oformat binary -o "$work/probe.bin" "$work/probe.o"
head -c 512 "$work/probe.bin" > "$work/probe.img"

run_one() {
	label=$1 cpu=$2
	out=$(timeout 20 qemu-system-x86_64 -machine pc -accel kvm -cpu "$cpu" \
		-nographic -m 512 -drive file="$work/probe.img",format=raw,if=ide 2>&1 |
		tr -d '\000' | tr '\r' '\n' | awk '
			/Booting from Hard Disk/ { sub(/.*Booting from Hard Disk\.*/, ""); if (length($0)) print; started=1; next }
			started && $0 != "." && $0 != "" { print }
		' | grep -Ev 'qemu-system|SeaBIOS|iPXE|^Press')
	vendor=$(printf '%s\n' "$out" | sed -n 1p)
	brand=$(printf '%s\n' "$out" | sed -n 2p | sed 's/ *$//')
	leaf1=$(printf '%s\n' "$out" | sed -n 3p)
	printf '%-8s vendor=%-13s leaf1=%s\n         brand=%s\n' "$label" "$vendor" "$leaf1" "$brand"
	eval "${label}_vendor=\$vendor" "${label}_leaf1=\$leaf1" "${label}_brand=\$brand"
}

run_one host "host"
run_one mask "$MASK"

fail=0
[ "$mask_vendor" = "GenuineIntel" ] || { echo "FAIL spoofed vendor: $mask_vendor"; fail=1; }
[ "$mask_leaf1" = "000906ea" ] || { echo "FAIL spoofed leaf1: $mask_leaf1 (want 000906ea)"; fail=1; }
case "$mask_brand" in
	"Intel(R) Core(TM) i7-9750H CPU @ 2.60GHz"*) ;;
	*) echo "FAIL spoofed brand: $mask_brand"; fail=1 ;;
esac
[ "$fail" -eq 0 ] && echo "CPU_MASK failures=0 pass=1"
exit "$fail"
