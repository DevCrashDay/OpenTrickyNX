    .text
    .global _start
_start:
    mov 4(%esp), %eax        /* argc unused; iterations from a fixed value */
    pushl $ITERS
    call run
    add $4, %esp
    /* write the 8 hex digits of eax */
    mov %eax, %edx
    lea buf+8, %edi
    mov $8, %ecx
1:  mov %edx, %eax
    and $15, %eax
    movb hex(%eax), %al
    dec %edi
    movb %al, (%edi)
    shr $4, %edx
    loop 1b
    mov $4, %eax
    mov $1, %ebx
    lea buf, %ecx
    mov $9, %edx
    int $0x80
    mov $1, %eax
    xor %ebx, %ebx
    int $0x80
    .data
hex: .ascii "0123456789abcdef"
buf: .ascii "00000000\n"
