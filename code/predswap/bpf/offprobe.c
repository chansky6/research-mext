// SPDX-License-Identifier: GPL-2.0
// offsetof 探针:在目标内核的头文件上编译,打印 predswap 需要的内核结构偏移。
#include <stdio.h>
#include <stddef.h>
#include <linux/mm_types.h>
#include <linux/sched.h>

int main(void)
{
    printf("VMF_ADDR_OFF=%zu\n", offsetof(struct vm_fault, address));
    printf("VMF_SIZE=%zu\n", sizeof(struct vm_fault));
    printf("TASK_PID_OFF=%zu\n", offsetof(struct task_struct, pid));
    return 0;
}
