/*
STALL_BACKTRACE.C

A thread's backtrace on SIGUSR2, for a game that hangs where no debugger may
attach (a container without CAP_SYS_PTRACE, where gdb -p is refused and
SIGQUIT's core may not be written), and where running it under gdb changes
it too much to hang (every SIGSEGV the memory watch takes stops it in gdb).
Preloaded into the game, it writes the backtrace of whichever thread takes
SIGUSR2 to stderr (the game's log), raw addresses, for addr2line on the
build afterwards:

    stall-backtrace: thread 1234
    stall-backtrace: 0x0804abcd
    ...

tools/skate_mp_loop.sh builds it (-m32, as the game) and sends the signal
to every thread of a host that stops logging, with tgkill. The game itself
does not use SIGUSR2.
*/

#define _GNU_SOURCE
#include <execinfo.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

static void stall_backtrace(int signal_number)
{
	void *frames[64];
	char line[64];
	int count, index, length;

	(void)signal_number;
	count = backtrace(frames, 64);
	length = snprintf(line, sizeof(line), "stall-backtrace: thread %ld\n", (long)syscall(SYS_gettid));
	if (write(2, line, (size_t)length) < 0)
		return;
	for (index = 0; index < count; index++)
	{
		length = snprintf(line, sizeof(line), "stall-backtrace: %p\n", frames[index]);
		if (write(2, line, (size_t)length) < 0)
			return;
	}
}

__attribute__((constructor)) static void stall_backtrace_install(void)
{
	struct sigaction action;
	void *frames[1];

	/* (backtrace loads libgcc the first time: not in the handler) */
	backtrace(frames, 1);
	memset(&action, 0, sizeof(action));
	action.sa_handler = stall_backtrace;
	action.sa_flags = SA_RESTART;
	sigemptyset(&action.sa_mask);
	sigaction(SIGUSR2, &action, NULL);
}
