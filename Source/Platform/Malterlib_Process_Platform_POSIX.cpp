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

#include "Malterlib_Process_Platform_POSIX.h"

#include <Mib/Core/PlatformSpecific/PosixErrNo>
#include "../Malterlib_Process_Platform.h"

// *************************************************************************************************************************
// Misc POSIX or Similar Function Implementation
// *************************************************************************************************************************

NStr::CStr NMib::NProcess::NPlatform::fg_Process_GetComputerDomain()
{
	char Hostname[_POSIX_HOST_NAME_MAX];

	int Result = gethostname(Hostname, _POSIX_HOST_NAME_MAX);

	if (Result == -1)
		DMibError(NMib::NPlatform::fg_FormatErrno("gethostname (get computer domain)", errno));

	NMib::NStr::CStr HostnameStr = Hostname;

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
	char Hostname[_POSIX_HOST_NAME_MAX];

	int Result = gethostname(Hostname, _POSIX_HOST_NAME_MAX);

	if (Result == -1)
		DMibError(NMib::NPlatform::fg_FormatErrno("gethostname (get computer name)", errno));

	NMib::NStr::CStr HostnameStr = Hostname;

	NMib::NStr::CStr ComputerName = fg_GetStrSep(HostnameStr, ".");
	if (HostnameStr == "local")
		return ComputerName + "." + HostnameStr;
	
	return ComputerName;
}

NStr::CStr NMib::NProcess::NPlatform::fg_Process_GetHostName()
{
	char Hostname[_POSIX_HOST_NAME_MAX];

	int Result = gethostname(Hostname, _POSIX_HOST_NAME_MAX);

	if (Result == -1)
		DMibError(NMib::NPlatform::fg_FormatErrno("gethostname (get host name)", errno));

	NMib::NStr::CStr HostnameStr = Hostname;
	
	return HostnameStr;
}

mint NMib::NProcess::NPlatform::fg_Process_GetCurrentUID()
{
	return getpid();
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

static NMib::NAggregate::TCAggregate<NMib::NFunction::TCFunction<void ()>> gs_TerminationFunction = {DAggregateInit};

NMib::COnScopeExitShared NMib::NProcess::NPlatform::fg_Process_WaitForTermination(NFunction::TCFunction<void ()> &&_fOnTerminate)
{
	if (gs_TerminationFunction.f_IsConstructed())
		DMibError("You can only install one wait for termination handler");

	gs_TerminationFunction.f_Construct(fg_Move(_fOnTerminate));

	static auto fSigTermHandler = [](int const _Signal)
		{
			(*gs_TerminationFunction)();
		}
	;

	auto fSigterm = signal(SIGTERM, (sig_t)fSigTermHandler);
	auto fSigint = signal(SIGINT, (sig_t)fSigTermHandler);

	return g_OnScopeExitShared > []
		{
			signal(SIGTERM, fSigterm);
			signal(SIGINT, fSigint);
			gs_TerminationFunction.f_Destruct();
		}
	;
}

bint NMib::NProcess::NPlatform::fg_Process_GetProcessIsParentProcess(mint _ProcessID)
{
	return getppid() == _ProcessID;
}

void NMib::NProcess::NPlatform::fg_Process_SetPriority(uint16 _Priority)
{
	int32 NiceProirity = (-int32(_Priority)*20 - 1) / int32(0x8000) + 20;
	if (setpriority(PRIO_PROCESS, getpid(), NiceProirity))
	{
		DMibError(NMib::NPlatform::fg_FormatErrno(NMib::NStr::CStrNonTracked::CFormat("setpriority({}) when setting process priority") << NiceProirity, errno));
	}
}

