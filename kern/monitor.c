// Simple command-line kernel monitor useful for
// controlling the kernel and exploring the system interactively.

#include <inc/stdio.h>
#include <inc/string.h>
#include <inc/memlayout.h>
#include <inc/assert.h>
#include <inc/x86.h>

#include <kern/console.h>
#include <kern/monitor.h>
#include <kern/kdebug.h>

#define CMDBUF_SIZE	80	// enough for one VGA text line
#define FN_NAME_MAX_LEN	256


struct Command {
	const char *name;
	const char *desc;
	// return -1 to force monitor to exit
	int (*func)(int argc, char** argv, struct Trapframe* tf);
};

static struct Command commands[] = {
	{ "help", "Display this list of commands", mon_help },
	{ "kerninfo", "Display information about the kernel", mon_kerninfo },
	{ "backtrace", "Display stack backtrace", mon_backtrace },
};
#define NCOMMANDS (sizeof(commands)/sizeof(commands[0]))

/***** Implementations of basic kernel monitor commands *****/

int
mon_help(int argc, char **argv, struct Trapframe *tf)
{
	int i;

	for (i = 0; i < NCOMMANDS; i++)
		cprintf("%s - %s\n", commands[i].name, commands[i].desc);
	return 0;
}

int
mon_kerninfo(int argc, char **argv, struct Trapframe *tf)
{
	extern char _start[], entry[], etext[], edata[], end[];

	cprintf("Special kernel symbols:\n");
	cprintf("  _start                  %08x (phys)\n", _start);
	cprintf("  entry  %08x (virt)  %08x (phys)\n", entry, entry - KERNBASE);
	cprintf("  etext  %08x (virt)  %08x (phys)\n", etext, etext - KERNBASE);
	cprintf("  edata  %08x (virt)  %08x (phys)\n", edata, edata - KERNBASE);
	cprintf("  end    %08x (virt)  %08x (phys)\n", end, end - KERNBASE);
	cprintf("Kernel executable memory footprint: %dKB\n",
		ROUNDUP(end - entry, 1024) / 1024);
	return 0;
}

void print_curr_trace() {
	uint32_t prev_ebp;
	uint32_t prev_eip;
	uint32_t prev_args[5];
	struct Eipdebuginfo info;

	__asm __volatile("movl (%%ebp),%0" : "=r" (prev_ebp));
	__asm __volatile("movl 0x4(%%ebp),%0" : "=r" (prev_eip));
	__asm __volatile("movl 0x8(%1),%0" : "=r" (prev_args[0]) : "r" (prev_ebp));
	__asm __volatile("movl 0xC(%1),%0" : "=r" (prev_args[1]) : "r" (prev_ebp));
	__asm __volatile("movl 0x10(%1),%0" : "=r" (prev_args[2]) : "r" (prev_ebp));
	__asm __volatile("movl 0x14(%1),%0" : "=r" (prev_args[3]) : "r" (prev_ebp));
	__asm __volatile("movl 0x18(%1),%0" : "=r" (prev_args[4]) : "r" (prev_ebp));

	cprintf("  ebp %08x eip %08x args %08x %08x %08x %08x %08x\n", prev_ebp, prev_eip, prev_args[0], prev_args[1], prev_args[2], prev_args[3], prev_args[4]);

	if(!debuginfo_eip(prev_eip, &info)){
		uint32_t fn_addr = prev_eip-(uint32_t)info.eip_fn_addr;
		cprintf("\t%s:%d: %.*s+%d\n", info.eip_file, info.eip_line, info.eip_fn_namelen, info.eip_fn_name, fn_addr);
	}
}

int
mon_backtrace(int argc, char **argv, struct Trapframe *tf)
{
	extern char bootstacktop[];

	int i;
	int j;
	uint32_t prev_ebp;
	uint32_t prev_eip;
	uint32_t prev_args[5];
	uint32_t curr_bp;
	struct Eipdebuginfo info;
	char dbg_fn_name[FN_NAME_MAX_LEN];

	cprintf("Stack backtrace:\n");

	print_curr_trace();

	__asm __volatile("mov %%ebp, %0" : "=r" (curr_bp));

	while(curr_bp != (uint32_t)bootstacktop-8) {
		__asm __volatile("movl (%1),%0" : "=r" (prev_ebp) : "r" (curr_bp));
		__asm __volatile("movl 0x4(%1),%0" : "=r" (prev_eip) : "r" (curr_bp));
		__asm __volatile("movl 0x8(%1),%0" : "=r" (prev_args[0]) : "r" (prev_ebp));
		__asm __volatile("movl 0xC(%1),%0" : "=r" (prev_args[1]) : "r" (prev_ebp));
		__asm __volatile("movl 0x10(%1),%0" : "=r" (prev_args[2]) : "r" (prev_ebp));
		__asm __volatile("movl 0x14(%1),%0" : "=r" (prev_args[3]) : "r" (prev_ebp));
		__asm __volatile("movl 0x18(%1),%0" : "=r" (prev_args[4]) : "r" (prev_ebp));

		cprintf("  ebp %08x eip %08x args %08x %08x %08x %08x %08x\n", prev_ebp, prev_eip, prev_args[0], prev_args[1], prev_args[2], prev_args[3], prev_args[4]);

		if(!debuginfo_eip(prev_eip, &info)){
			uint32_t fn_addr = prev_eip-(uint32_t)info.eip_fn_addr;
			cprintf(" \t%s:%d: %.*s+%d\n", info.eip_file, info.eip_line, info.eip_fn_namelen, info.eip_fn_name, fn_addr);
		}

		__asm __volatile("mov (%1), %0" : "=r" (curr_bp) : "r" (curr_bp));
	}

	return 0;
}



/***** Kernel monitor command interpreter *****/

#define WHITESPACE "\t\r\n "
#define MAXARGS 16

static int
runcmd(char *buf, struct Trapframe *tf)
{
	int argc;
	char *argv[MAXARGS];
	int i;

	// Parse the command buffer into whitespace-separated arguments
	argc = 0;
	argv[argc] = 0;
	while (1) {
		// gobble whitespace
		while (*buf && strchr(WHITESPACE, *buf))
			*buf++ = 0;
		if (*buf == 0)
			break;

		// save and scan past next arg
		if (argc == MAXARGS-1) {
			cprintf("Too many arguments (max %d)\n", MAXARGS);
			return 0;
		}
		argv[argc++] = buf;
		while (*buf && !strchr(WHITESPACE, *buf))
			buf++;
	}
	argv[argc] = 0;

	// Lookup and invoke the command
	if (argc == 0)
		return 0;
	for (i = 0; i < NCOMMANDS; i++) {
		if (strcmp(argv[0], commands[i].name) == 0)
			return commands[i].func(argc, argv, tf);
	}
	cprintf("Unknown command '%s'\n", argv[0]);
	return 0;
}

void
monitor(struct Trapframe *tf)
{
	char *buf;

	cprintf("Welcome to the JOS kernel monitor!\n");
	cprintf("Type 'help' for a list of commands.\n");


	while (1) {
		buf = readline("K> ");
		if (buf != NULL)
			if (runcmd(buf, tf) < 0)
				break;
	}
}
