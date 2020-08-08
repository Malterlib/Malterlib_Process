// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

using namespace NMib;

#include <signal.h>
#include <pwd.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>
#ifdef DPlatformFamily_Linux
#include <sys/time.h>
#include <sys/resource.h>
#endif
#ifdef DPlatformFamily_OSX
#include <mach/mach_init.h>
#include <mach/thread_policy.h>
#include <mach/task_policy.h>
#include <mach/task.h>
#endif

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

#include "Malterlib_Process_Platform_POSIX.h"

#include <Mib/Core/PlatformSpecific/PosixErrNo>
#include "../Malterlib_Process_Platform.h"

// *************************************************************************************************************************
// Misc POSIX or Similar Function Implementation
// *************************************************************************************************************************

NStr::CStr NMib::NProcess::NPlatform::fg_Process_GetComputerDomain()
{
	NMib::NStr::CStr HostnameStr = fg_Process_GetFullyQualiedHostName();

	fg_GetStrSep(HostnameStr, ".");

	return HostnameStr;
}

NStr::CStr NMib::NProcess::NPlatform::fg_Process_GetUserName()
{
	auto UserID = getuid();
	auto pUserPassword = getpwuid(UserID);
	if (pUserPassword && pUserPassword->pw_name)
    	return NMib::NStr::CStr(pUserPassword->pw_name);
	return NMib::NStr::CStr::fs_ToStr(UserID);
}

#ifndef DPlatformFamily_OSX

void *NMib::NProcess::NPlatform::fg_Process_Pause(mint _ProcessID)
{
	if (kill(_ProcessID, SIGSTOP))
		return nullptr;

	return (void *)1;
}
void NMib::NProcess::NPlatform::fg_Process_Resume(mint _ProcessID, void *_pPauseToken)
{
	if (!_pPauseToken)
		return;

	kill(_ProcessID, SIGCONT);
}

#endif

void NMib::NProcess::NPlatform::fg_Process_Terminate(mint _ProcessID)
{
	if (kill(_ProcessID, SIGKILL))
		DMibError(NMib::NPlatform::fg_FormatErrno(NMib::NStr::CStr::CFormat("kill({}, SIGKILL) when terminating process") << _ProcessID, errno));
}

void NMib::NProcess::NPlatform::fg_Process_Stop(mint _ProcessID)
{
	if (kill(_ProcessID, SIGTERM))
		DMibError(NMib::NPlatform::fg_FormatErrno(NMib::NStr::CStr::CFormat("kill({}, SIGTERM) when stopping process") << _ProcessID, errno));
}

NStr::CStr NMib::NProcess::NPlatform::fg_Process_GetComputerName()
{
	char Hostname[_POSIX_HOST_NAME_MAX];

	int Result = gethostname(Hostname, _POSIX_HOST_NAME_MAX);

	if (Result == -1)
		DMibError(NMib::NPlatform::fg_FormatErrno("gethostname (get computer name)", errno));

	NMib::NStr::CStr HostnameStr = Hostname;

	NMib::NStr::CStr ComputerName = fg_GetStrSep(HostnameStr, ".");
	
	return ComputerName;
}

NStr::CStr NMib::NProcess::NPlatform::fg_Process_GetComputerAddress()
{
	NMib::NStr::CStr HostnameStr = fg_Process_GetHostName();

	NMib::NStr::CStr ComputerName = fg_GetStrSep(HostnameStr, ".");
	if (HostnameStr == "local")
		return ComputerName + "." + HostnameStr;
	
	return ComputerName;
}

NStr::CStr NMib::NProcess::NPlatform::fg_Process_GetHostName()
{
	using namespace NMib::NStr;

	char Hostname[_POSIX_HOST_NAME_MAX];

	int Result = gethostname(Hostname, _POSIX_HOST_NAME_MAX);

	if (Result == -1)
		DMibError(NMib::NPlatform::fg_FormatErrno("gethostname (get host name)", errno));

	return Hostname;
}

NStr::CStr NMib::NProcess::NPlatform::fg_Process_GetFullyQualiedHostName()
{
	using namespace NMib::NStr;

	char Hostname[_POSIX_HOST_NAME_MAX];

	int Result = gethostname(Hostname, _POSIX_HOST_NAME_MAX);

	if (Result == -1)
		DMibError(NMib::NPlatform::fg_FormatErrno("gethostname (get host name)", errno));

#ifdef DPlatformFamily_OSX
	return Hostname;
#else
	struct addrinfo HintAddrInfo;
	NMemory::fg_MemClear(HintAddrInfo);

	HintAddrInfo.ai_socktype = SOCK_DGRAM;
	HintAddrInfo.ai_flags = AI_CANONNAME;

	addrinfo *pAddrInfo = nullptr;

	Result = getaddrinfo(Hostname, NULL, &HintAddrInfo, &pAddrInfo);

	if (Result == -1)
		DMibError(NMib::NPlatform::fg_FormatErrno("getaddrinfo (get host name)", errno));

	CStr FullyQualifiedName = pAddrInfo->ai_canonname;

	freeaddrinfo(pAddrInfo);

	return FullyQualifiedName;
#endif
}

mint NMib::NProcess::NPlatform::fg_Process_GetCurrentUID()
{
	return getpid();
}

mint NMib::NProcess::NPlatform::fg_Process_GetCurrentGroupUID()
{
	return getpgid(0);
}

void NMib::NProcess::NPlatform::fg_Process_AbortWaitForTermination()
{
	kill(getpid(), SIGTERM);
}

void NMib::NProcess::NPlatform::fg_Process_WaitForTermination()
{
    sigset_t WaitSet;

    sigemptyset(&WaitSet);
    sigaddset(&WaitSet, SIGTERM);
    sigaddset(&WaitSet, SIGINT);

    sigprocmask(SIG_BLOCK, &WaitSet, nullptr);
	
	for (;;)
	{
		errno = 0;

		int Signal = 0;
		sigwait(&WaitSet, &Signal);
		
		if (errno == EINTR)
			continue;
		else if (errno != 0)
			DMibError(NMib::NPlatform::fg_FormatErrno("sigwait", errno));
		else
			break;
	}
}

constinit static NMib::NStorage::TCAggregate<NMib::NFunction::TCFunction<void ()>> gs_TerminationFunction = {DAggregateInit};

NMib::COnScopeExitShared NMib::NProcess::NPlatform::fg_Process_WaitForTermination(NFunction::TCFunction<void ()> &&_fOnTerminate)
{
	if (gs_TerminationFunction.f_IsConstructed())
		DMibError("You can only install one wait for termination handler");

	gs_TerminationFunction.f_Construct(fg_Move(_fOnTerminate));

	static auto fSigTermHandler = [](int const _Signal)
		{
			sigset_t WaitSet;
			sigset_t OldSet;
			sigemptyset(&WaitSet);
			sigaddset(&WaitSet, SIGTERM);
			sigaddset(&WaitSet, SIGINT);
			sigprocmask(SIG_BLOCK, &WaitSet, &OldSet);

			auto Cleanup = g_OnScopeExit > [&]
				{
					sigprocmask(SIG_SETMASK, &OldSet, nullptr);
				}
			;

			(*gs_TerminationFunction)();
		}
	;

	auto fSigterm = signal(SIGTERM, (sig_t)fSigTermHandler);
	auto fSigint = signal(SIGINT, (sig_t)fSigTermHandler);

	return g_OnScopeExitShared > [fSigterm, fSigint]
		{
			signal(SIGTERM, fSigterm);
			signal(SIGINT, fSigint);
			gs_TerminationFunction.f_Destruct();
		}
	;
}

bool NMib::NProcess::NPlatform::fg_Process_GetProcessIsParentProcess(mint _ProcessID)
{
	return getppid() == _ProcessID;
}

namespace
{
#ifdef DPlatformFamily_Linux
	static constexpr int32 gc_MaxNice = 19;
#else
	static constexpr int32 gc_MaxNice = 20;
#endif

	int32 fg_Process_GetNice(EExecutionPriority _Priority)
	{
		if (_Priority < EExecutionPriority_Normal)
			return ((0x8000 - int32(_Priority)) * gc_MaxNice) / 0x8000;
		else
			return (((0x8000 - int32(_Priority + 1)) * 20) / 0x8000);
	}

	int32 fg_Process_GetNiceReverse(int32 _Priority)
	{
		if (_Priority < 0)
			return EExecutionPriority((0x8000 - (_Priority * 0x8000) / 20) - 1);
		else
			return EExecutionPriority(0x8000 - (_Priority * 0x8000) / gc_MaxNice);
	}
}

EExecutionPriority NMib::NProcess::NPlatform::fg_Process_GetPriority()
{
	return EExecutionPriority(fg_Process_GetNiceReverse(getpriority(PRIO_PROCESS, getpid())));
}

void NMib::NProcess::NPlatform::fg_Process_SetPriority(EExecutionPriority _Priority)
{
	// Best effort for setting priority
	for (int32 NiceProirity = fg_Process_GetNice(_Priority); NiceProirity <= 20; ++NiceProirity)
	{
		if (!setpriority(PRIO_PROCESS, getpid(), NiceProirity))
			break;
	}

#ifdef DPlatformFamily_OSX
	{
		struct task_category_policy TaskCategoryPolity;
		if (_Priority > EExecutionPriority_Normal)
			TaskCategoryPolity.role = TASK_FOREGROUND_APPLICATION;
		else if (_Priority == EExecutionPriority_Normal)
			TaskCategoryPolity.role = TASK_UNSPECIFIED;
		else
			TaskCategoryPolity.role = TASK_BACKGROUND_APPLICATION;
		task_policy_set(mach_task_self(), TASK_CATEGORY_POLICY, (thread_policy_t)&TaskCategoryPolity, TASK_CATEGORY_POLICY_COUNT);
	}
#endif
}
