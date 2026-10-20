    .text
    .global __aarch64_read_tp
__aarch64_read_tp:
    mrs x0, tpidr_el0
    ret
