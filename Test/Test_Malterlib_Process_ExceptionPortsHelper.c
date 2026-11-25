// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Does not use Malterlib, as the exception handler of the Malterlib memory manager replaces the EXC_BAD_ACCESS port
// a process inherits.

#include <crt_externs.h>
#include <errno.h>
#include <mach/mach.h>
#include <pthread.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

// The kernel, or the Malterlib handler that forwards to the port, waits for the reply to an exception. Destroying the
// request destroys its reply right, which fails the exception, so it goes on to the next handler as without this port.
static void *fg_FailExceptions(void *_pPort)
{
	mach_port_t Port = (mach_port_t)(uintptr_t)_pPort;

	for (;;)
	{
		union
		{
			mach_msg_header_t m_Header;
			uint8_t m_Buffer[4096];
		} Message;

		mach_msg_return_t Result = mach_msg(&Message.m_Header, MACH_RCV_MSG, 0, sizeof(Message), Port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
		if (Result == MACH_MSG_SUCCESS)
			mach_msg_destroy(&Message.m_Header);
		else if (Result != MACH_RCV_TOO_LARGE && Result != MACH_RCV_INTERRUPTED) // A message too large is destroyed by the receive
			return NULL;
	}
}

// The executable inherits the EXC_BAD_ACCESS port set here. The receive right is held, and the exceptions sent to it are
// failed, until the executable exits, so a crash of the executable ends it with a signal.
static int fg_Spawn(exception_behavior_t _Behavior, char **_pArguments)
{
	mach_port_t Port = MACH_PORT_NULL;
	kern_return_t Result = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &Port);
	if (Result != KERN_SUCCESS)
	{
		fprintf(stderr, "mach_port_allocate failed: %d\n", Result);
		return 1;
	}

	Result = mach_port_insert_right(mach_task_self(), Port, Port, MACH_MSG_TYPE_MAKE_SEND);
	if (Result != KERN_SUCCESS)
	{
		fprintf(stderr, "mach_port_insert_right failed: %d\n", Result);
		return 1;
	}

	Result = task_set_exception_ports(mach_task_self(), EXC_MASK_BAD_ACCESS, Port, _Behavior, MACHINE_THREAD_STATE);
	if (Result != KERN_SUCCESS)
	{
		fprintf(stderr, "task_set_exception_ports failed: %d\n", Result);
		return 1;
	}

	pthread_t FailExceptionsThread;
	int Error = pthread_create(&FailExceptionsThread, NULL, fg_FailExceptions, (void *)(uintptr_t)Port);
	if (Error)
	{
		fprintf(stderr, "pthread_create failed: %s\n", strerror(Error));
		return 1;
	}

	pid_t Pid = -1;
	Error = posix_spawn(&Pid, _pArguments[0], NULL, NULL, _pArguments, *_NSGetEnviron());
	if (Error)
	{
		fprintf(stderr, "posix_spawn of %s failed: %s\n", _pArguments[0], strerror(Error));
		return 1;
	}

	int Status = 0;
	while (waitpid(Pid, &Status, 0) < 0)
	{
		if (errno != EINTR)
		{
			fprintf(stderr, "waitpid failed: %s\n", strerror(errno));
			return 1;
		}
	}

	if (WIFSIGNALED(Status))
	{
		fprintf(stderr, "%s was terminated by signal %d\n", _pArguments[0], WTERMSIG(Status));
		return 1;
	}

	return WEXITSTATUS(Status);
}

static int fg_Report(void)
{
	exception_mask_t Masks[EXC_TYPES_COUNT];
	mach_msg_type_number_t nMasks = EXC_TYPES_COUNT;
	mach_port_t Ports[EXC_TYPES_COUNT];
	exception_behavior_t Behaviors[EXC_TYPES_COUNT];
	thread_state_flavor_t Flavors[EXC_TYPES_COUNT];

	kern_return_t Result = task_get_exception_ports(mach_task_self(), EXC_MASK_BAD_ACCESS, Masks, &nMasks, Ports, Behaviors, Flavors);
	if (Result != KERN_SUCCESS)
	{
		fprintf(stderr, "task_get_exception_ports failed: %d\n", Result);
		return 1;
	}

	for (mach_msg_type_number_t iMask = 0; iMask < nMasks; ++iMask)
	{
		if (!(Masks[iMask] & EXC_MASK_BAD_ACCESS) || !MACH_PORT_VALID(Ports[iMask]))
			continue;

		printf("Port %u %d\n", (unsigned)Behaviors[iMask], Flavors[iMask]);
		return 0;
	}

	printf("Null\n");
	return 0;
}

int main(int _nArguments, char **_pArguments)
{
	if (_nArguments >= 4 && strcmp(_pArguments[1], "--spawn") == 0)
		return fg_Spawn((exception_behavior_t)strtoul(_pArguments[2], NULL, 10), _pArguments + 3);

	if (_nArguments == 2 && strcmp(_pArguments[1], "--report") == 0)
		return fg_Report();

	fprintf(stderr, "Usage: %s --spawn <Behavior> <Executable> [<Arguments>...] | --report\n", _pArguments[0]);
	return 1;
}
