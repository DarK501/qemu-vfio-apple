/*
 * Diagnostic loaded at GPA 0x1000. Page tables at 0x2000-0x4fff map
 * the first 2 MiB and the test buffer at linear address 0x40000000.
 * The mailbox at 0x5000 coordinates with a paused host.
 */
.code16
.global _start
_start:
    cli
    xor %ax, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov $0x7000, %sp
    in $0x92, %al
    or $2, %al
    out %al, $0x92
    lgdt gdtr
    mov %cr0, %eax
    or $1, %eax
    mov %eax, %cr0
    ljmpl $0x08, $protected

.code32
protected:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov %cr4, %eax
    or $0x20, %eax
    mov %eax, %cr4
    mov $0x2000, %eax
    mov %eax, %cr3
    mov %cr0, %eax
    or $0x80000000, %eax
    mov %eax, %cr0
    jmp 1f
1:
    mov $0x40000000, %edi
    movl $0xe5f60718, (%edi)
    movl $0xa1b2c3d4, 4(%edi)
    movl $1, 0x5000
2:
    cmpl $2, 0x5000
    jne 2b
    mov (%edi), %eax
    mov %eax, 0x5008
    mov 4(%edi), %eax
    mov %eax, 0x500c
    movl $3, 0x5000
3:
    hlt
    jmp 3b

.balign 8
gdt:
    .quad 0
    .quad 0x00cf9a000000ffff
    .quad 0x00cf92000000ffff
gdt_end:
gdtr:
    .word gdt_end - gdt - 1
    .long gdt
