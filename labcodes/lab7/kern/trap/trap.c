#include <defs.h>
#include <mmu.h>
#include <memlayout.h>
#include <clock.h>
#include <trap.h>
#include <x86.h>
#include <stdio.h>
#include <assert.h>
#include <console.h>
#include <vmm.h>
#include <swap.h>
#include <kdebug.h>
#include <unistd.h>
#include <syscall.h>
#include <error.h>
#include <sched.h>
#include <sync.h>
#include <proc.h>
#include <string.h>

#define TICK_NUM 100

static void print_ticks() {
    cprintf("%d ticks\n",TICK_NUM);
#ifdef DEBUG_GRADE
    cprintf("End of Test.\n");
    panic("EOT: kernel seems ok.");
#endif
}

/* *
 * Interrupt descriptor table:
 *
 * Must be built at run time because shifted function addresses can't
 * be represented in relocation records.
 * */
static struct gatedesc idt[256] = {{0}};

static struct pseudodesc idt_pd = {
    sizeof(idt) - 1, (uintptr_t)idt
};

/* idt_init - initialize IDT to each of the entry points in kern/trap/vectors.S */
void idt_init(void)
{
    /* LAB1 YOUR CODE : STEP 2 */
    /* (1) Where are the entry addrs
     *     All ISR's entry addrs are stored in __vectors. where is uintptr_t __v of each Interrupt Service Routine (ISR)?ectors[] ?
     *     __vectors[] is in kern/trap/vector.S which is produced by tools/vector.c
     *     (try "make" command in lab1, then you will find vector.S in kern/trap DIR)
     *     You can use  "extern uintptr_t __vectors[];" to define this extern variable which will be used later.
     * (2) Now you should setup the entries of ISR in Interrupt Description Table (IDT).
     *     Can you see idt[256] in this file? Yes, it's IDT! you can use SETGATE macro to setup each item of IDT
     * (3) After setup the contents of IDT, you will let CPU know where is the IDT by using 'lidt' instruction.
     *     You don't know the meaning of this instruction? just google it! and check the libs/x86.h to know more.
     *     Notice: the argument of lidt is idt_pd. try to find it!
     */
    // 根据提示，首先要声明__vectors，extern是外部变量声明，__vectors是通过tools/vector.c生成的vectors.S里面定义的
    extern uintptr_t __vectors[];
    // 对256个中断向量表初始化
    int i;
    for (i = 0; i < (sizeof(idt) / (sizeof(struct gatedesc))); i++) {
        // idt数据里面的每一个，也可以用指针表示，
        // 0表示是一个interrupt gate
        // segment selector设置为GD_KTEXT（代码段）
        // offset设置为__vectors对应的内容
        // DPL设置为0
        SETGATE(idt[i], 0, GD_KTEXT, __vectors[i], DPL_KERNEL);
    }

        // 再把从用户态切换到内核态使用的Segment Descriptor改一下
        // 需要注意的是，我们使用的segment都是一样的，都是GD_KTEXT
        // 而有一点不同的是这里的DPL是DPL_USER，即从user->kernel时，需要的该段的权限级别
        // 因为Privilege Check需要满足：DPL >= max {CPL, RPL}
        // 所以如果不单独改这个会造成Privilege Check失败，无法正确处理user->kernel的流程

        // lab1后可能已经用不到了
        SETGATE(idt[T_SWITCH_TOK], 0, GD_KTEXT, __vectors[T_SWITCH_TOK], DPL_USER);
        // lab5补充，从用户态切换到内核态的idt设置
        // SETGATE(idt[T_SYSCALL], 1, GD_KTEXT, __vectors[T_SYSCALL], DPL_USER);
        SETGATE(idt[T_SYSCALL], 1, GD_KTEXT, __vectors[T_SYSCALL], DPL_USER);
        // 通过lidt加载
        lidt(&idt_pd);
}

static const char *
trapname(int trapno) {
    static const char * const excnames[] = {
        "Divide error",
        "Debug",
        "Non-Maskable Interrupt",
        "Breakpoint",
        "Overflow",
        "BOUND Range Exceeded",
        "Invalid Opcode",
        "Device Not Available",
        "Double Fault",
        "Coprocessor Segment Overrun",
        "Invalid TSS",
        "Segment Not Present",
        "Stack Fault",
        "General Protection",
        "Page Fault",
        "(unknown trap)",
        "x87 FPU Floating-Point Error",
        "Alignment Check",
        "Machine-Check",
        "SIMD Floating-Point Exception"
    };

    if (trapno < sizeof(excnames)/sizeof(const char * const)) {
        return excnames[trapno];
    }
    if (trapno >= IRQ_OFFSET && trapno < IRQ_OFFSET + 16) {
        return "Hardware Interrupt";
    }
    return "(unknown trap)";
}

/* trap_in_kernel - test if trap happened in kernel */
bool
trap_in_kernel(struct trapframe *tf) {
    return (tf->tf_cs == (uint16_t)KERNEL_CS);
}

static const char *IA32flags[] = {
    "CF", NULL, "PF", NULL, "AF", NULL, "ZF", "SF",
    "TF", "IF", "DF", "OF", NULL, NULL, "NT", NULL,
    "RF", "VM", "AC", "VIF", "VIP", "ID", NULL, NULL,
};

void
print_trapframe(struct trapframe *tf) {
    cprintf("trapframe at %p\n", tf);
    print_regs(&tf->tf_regs);
    cprintf("  ds   0x----%04x\n", tf->tf_ds);
    cprintf("  es   0x----%04x\n", tf->tf_es);
    cprintf("  fs   0x----%04x\n", tf->tf_fs);
    cprintf("  gs   0x----%04x\n", tf->tf_gs);
    cprintf("  trap 0x%08x %s\n", tf->tf_trapno, trapname(tf->tf_trapno));
    cprintf("  err  0x%08x\n", tf->tf_err);
    cprintf("  eip  0x%08x\n", tf->tf_eip);
    cprintf("  cs   0x----%04x\n", tf->tf_cs);
    cprintf("  flag 0x%08x ", tf->tf_eflags);

    int i, j;
    for (i = 0, j = 1; i < sizeof(IA32flags) / sizeof(IA32flags[0]); i ++, j <<= 1) {
        if ((tf->tf_eflags & j) && IA32flags[i] != NULL) {
            cprintf("%s,", IA32flags[i]);
        }
    }
    cprintf("IOPL=%d\n", (tf->tf_eflags & FL_IOPL_MASK) >> 12);

    if (!trap_in_kernel(tf)) {
        cprintf("  esp  0x%08x\n", tf->tf_esp);
        cprintf("  ss   0x----%04x\n", tf->tf_ss);
    }
}

void
print_regs(struct pushregs *regs) {
    cprintf("  edi  0x%08x\n", regs->reg_edi);
    cprintf("  esi  0x%08x\n", regs->reg_esi);
    cprintf("  ebp  0x%08x\n", regs->reg_ebp);
    cprintf("  oesp 0x%08x\n", regs->reg_oesp);
    cprintf("  ebx  0x%08x\n", regs->reg_ebx);
    cprintf("  edx  0x%08x\n", regs->reg_edx);
    cprintf("  ecx  0x%08x\n", regs->reg_ecx);
    cprintf("  eax  0x%08x\n", regs->reg_eax);
}

struct trapframe switchk2u, *switchu2k;

/**
 *
 */
void switch2user(struct trapframe *tf)
{
    // eflags
    // 0x3000 = 00110000 00000000
    // 把nested task位置1，也就是可以嵌套
    tf->tf_eflags |= 0x3000;

    // USER_CS = 3 << 3 | 3 = 24 | 3 = 27 = 0x1B = 00011011;
    // 如果当前运行的程序不是在用户态的代码段执行（从内核切换过来肯定不会是）
    if (tf->tf_cs != USER_CS)
    {
        switchk2u = *tf;
        switchk2u.tf_cs = USER_CS;
        // 设置数据段为USER_DS
        switchk2u.tf_ds = switchk2u.tf_es = switchk2u.tf_ss = USER_DS;
        // 因为内存是从高到低，
        // 而这是从内核态切换到用户态（没有ss,sp）
        // (uint32_t)tf + sizeof(struct trapframe) - 8 即 tf->tf_esp的地址
        // 也就是switchk2u.tf_esp，指向旧的tf_esp的值
        switchk2u.tf_esp = (uint32_t)tf + sizeof(struct trapframe) - 8;

        //  eflags 设置IOPL
        switchk2u.tf_eflags | FL_IOPL_MASK;

        // (uint32_t *)tf是一个指针，指针的地址-1就
        // *((uint32_t *)tf - 1) 这个指针指向的地址设置为我们新樊笼出来的tss(switchk2u)

        *((uint32_t *)tf - 1) = (uint32_t)&switchk2u;
    }
}

void switch2kernel(struct trapframe *tf)
{
    if (tf->tf_cs != KERNEL_CS)
    {
        // 设置CS为 KERNEL_CS = 0x8 = 1000 =  00001|0|00 -> Index = 1, GDT, RPL = 0
        tf->tf_cs = KERNEL_CS;
        // KERNEL_DS = 00010|0|00 -> Index = 2, GDT, RPL = 0
        tf->tf_ds = tf->tf_es = KERNEL_DS;

        // FL_IOPL_MASK = 0x00003000 = 0011000000000000 = 00110000 00000000
        // I/O Privilege Level bitmask
        // tf->tf_eflags = (tf->tf_eflags) & (~FL_IOPL_MASK)
        // = (tf->tf_eflags) & (11111111 11111111 11001111 11111111)
        // 也就是把IOPL设置为0
        // IOPL(bits 12 and 13) [I/O privilege level field]
        // 指示当前运行任务的I/O特权级(I/O privilege level)，
        // 正在运行任务的当前特权级(CPL)必须小于或等于I/O特权级才能允许访问I/O地址空间。
        // 这个域只能在CPL为0时才能通过POPF以及IRET指令修改。
        tf->tf_eflags &= ~FL_IOPL_MASK;

        // 由于内存布局是从高到低，所以这里修改switchu2k，指向
        switchu2k = (struct trapframe *)(tf->tf_esp - (sizeof(struct trapframe) - 8));

        /* *
         * memmove - copies the values of @n bytes from the location pointed by @src to
         * the memory area pointed by @dst. @src and @dst are allowed to overlap.
         * @dst        pointer to the destination array where the content is to be copied
         * @src        pointer to the source of data to by copied
         * @n:        number of bytes to copy
         *
         * The memmove() function returns @dst.
         * */
        // 相当于是把tf，拷贝到switchu2k
        memmove(switchu2k, tf, sizeof(struct trapframe) - 8);

        // 修改tf - 1处，指向新的trapframe
        *((uint32_t *)tf - 1) = (uint32_t)switchu2k;
    }
}

static inline void
print_pgfault(struct trapframe *tf) {
    /* error_code:
     * bit 0 == 0 means no page found, 1 means protection fault
     * bit 1 == 0 means read, 1 means write
     * bit 2 == 0 means kernel, 1 means user
     * */
    cprintf("page fault at 0x%08x: %c/%c [%s].\n", rcr2(),
            (tf->tf_err & 4) ? 'U' : 'K',
            (tf->tf_err & 2) ? 'W' : 'R',
            (tf->tf_err & 1) ? "protection fault" : "no page found");
}

static int
pgfault_handler(struct trapframe *tf)
{
        // 页访问异常错误码有32位。
        // 位0为１表示对应物理页不存在；
        // 位１为１表示写异常（比如写了只读页；位２为１表示访问权限异常（比如用户态程序访问内核空间的数据）

        // CR2是页故障线性地址寄存器，保存最后一次出现页故障的全32位线性地址。
        // CR2用于发生页异常时报告出错信息。当发生页异常时，处理器把引起页异常的线性地址保存在CR2中。
        // 操作系统中对应的中断服务例程可以检查CR2的内容，从而查出线性地址空间中的哪个页引起本次异常。
        // check_mm_struct
    extern struct mm_struct *check_mm_struct;
    if(check_mm_struct !=NULL) { //used for test check_swap
            print_pgfault(tf);
        }
    struct mm_struct *mm;
    if (check_mm_struct != NULL) {
        assert(current == idleproc);
        mm = check_mm_struct;
    }
    else {
        if (current == NULL) {
            print_trapframe(tf);
            print_pgfault(tf);
            panic("unhandled page fault.\n");
        }
        mm = current->mm;
    }
    return do_pgfault(mm, tf->tf_err, rcr2());
}

static volatile int in_swap_tick_event = 0;
extern struct mm_struct *check_mm_struct;

static void
trap_dispatch(struct trapframe *tf) {
    char c;

    int ret=0;

    switch (tf->tf_trapno) {
    case T_PGFLT:  //page fault
        if ((ret = pgfault_handler(tf)) != 0) {
            print_trapframe(tf);
            if (current == NULL) {
                panic("handle pgfault failed. ret=%d\n", ret);
            }
            else {
                if (trap_in_kernel(tf)) {
                    panic("handle pgfault failed in kernel mode. ret=%d\n", ret);
                }
                cprintf("killed by kernel.\n");
                panic("handle user mode pgfault failed. ret=%d\n", ret); 
                do_exit(-E_KILLED);
            }
        }
        break;
    case T_SYSCALL:
        syscall();
        break;
    case IRQ_OFFSET + IRQ_TIMER:
#if 0
    LAB3 : If some page replacement algorithm(such as CLOCK PRA) need tick to change the priority of pages,
    then you can add code here. 
#endif
        /* LAB1 YOUR CODE : STEP 3 */
        /* handle the timer interrupt */
        /* (1) After a timer interrupt, you should record this event using a global variable (increase it), such as ticks in kern/driver/clock.c
         * (2) Every TICK_NUM cycle, you can print some info using a funciton, such as print_ticks().
         * (3) Too Simple? Yes, I think so!
         */
        /* LAB5 YOUR CODE */
        /* you should upate you lab1 code (just add ONE or TWO lines of code):
         *    Every TICK_NUM cycle, you should set current process's current->need_resched = 1
         */
        /* LAB6 YOUR CODE */
        /* you should upate you lab5 code
         * IMPORTANT FUNCTIONS:
	     * sched_class_proc_tick
         */ 
        ticks ++;
        // if (ticks % TICK_NUM == 0) {
        // // 设置当前的process current->need_resched = 1
        //     assert(current != NULL);
        //     // 设置need_resched = 1
        //     // current->need_resched = 1;
        //     // print_ticks();
        // }
        assert(current != NULL);
        sched_class_proc_tick(current);        
        /* LAB7 YOUR CODE */
        /* you should upate you lab6 code
         * IMPORTANT FUNCTIONS:
	     * run_timer_list
         */
        break;
    case IRQ_OFFSET + IRQ_COM1:
        c = cons_getc();
        cprintf("serial [%03d] %c\n", c, c);
        break;
    case IRQ_OFFSET + IRQ_KBD:
        c = cons_getc();
        cprintf("kbd [%03d] %c\n", c, c);

        /*********************/
        // Hardware Interrupt is different with software trap, so no need use temp stack

        // if keyboard input '3' it will go to USER mode

        if (c == '3')
        {
            switch2user(tf);
            // the status can show in trapframe,
            // however register value change at iret in trapentry.s,
            // so lab1_print_cur_status() does not work
            print_trapframe(tf);
            // lab1_print_cur_status();
        }

        // if keyboard input '0' it will go to Kernel mode
        if (c == '0')
        {

            switch2kernel(tf);
            print_trapframe(tf);
            // lab1_print_cur_status();
        }
        break;
    // LAB1 CHALLENGE 1 : YOUR CODE you should modify below codes.
    case T_SWITCH_TOU:
        // USER_CS = 3 << 3 | 3 = 24 | 3 = 27 = 0x1B = 00011011
        switch2user(tf);
        break;
    case T_SWITCH_TOK:
        // panic("T_SWITCH_** ??\n");
        switch2kernel(tf);
        break;
    case IRQ_OFFSET + IRQ_IDE1:
    case IRQ_OFFSET + IRQ_IDE2:
        /* do nothing */
        break;
    default:
        print_trapframe(tf);
        if (current != NULL) {
            cprintf("unhandled trap.\n");
            do_exit(-E_KILLED);
        }
        // in kernel, it must be a mistake
        panic("unexpected trap in kernel.\n");

    }
}

/* *
 * trap - handles or dispatches an exception/interrupt. if and when trap() returns,
 * the code in kern/trap/trapentry.S restores the old CPU state saved in the
 * trapframe and then uses the iret instruction to return from the exception.
 * */
void
trap(struct trapframe *tf) {
    // dispatch based on what type of trap occurred
    // used for previous projects
    if (current == NULL) {
        trap_dispatch(tf);
    }
    else {
        // keep a trapframe chain in stack
        struct trapframe *otf = current->tf;
        current->tf = tf;
    
        bool in_kernel = trap_in_kernel(tf);
    
        trap_dispatch(tf);
    
        current->tf = otf;
        if (!in_kernel) {
            if (current->flags & PF_EXITING) {
                do_exit(-E_KILLED);
            }
            if (current->need_resched) {
                schedule();
            }
        }
    }
}

