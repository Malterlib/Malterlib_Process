// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Process/ProcessLaunch>
#include <Mib/Core/PlatformSpecific/MacOSOSStatus>

#include <Security/Authorization.h>
#include <Security/Security.h>

#include "Malterlib_Process_Platform_POSIX_PlatformSpecific.h"

bool NMib::NProcess::NPlatform::fg_MacOS_LaunchExecutableWithRoot
	(
		NMib::NProcess::CProcessLaunchParams const &_Params
		, NAtomic::TCAtomic<pid_t> &o_PID
		, int &_hStdOutRead
		, int &_hStdInWrite
		, NMib::NStr::CStr& _Errors
	)
{
	NStr::CStr Executable = _Params.m_Target;

	NContainer::TCVector<NStr::CStr> Parameters;
	NContainer::TCVector<ch8 *> ParametersList;

	//ParametersList.f_Insert(Executable.f_GetStrUniqueWritable());
	NStr::CStr Params = _Params.m_Parameters;
	while (!Params.f_IsEmpty())
	{
		NStr::CStr Param = fg_GetStrSepEscaped(Params, " ");
		ParametersList.f_Insert(Parameters.f_Insert(Param).f_GetStrUniqueWritable());
	}

	if (_Params.m_bStdOutPID)
	{
		ParametersList.f_Insert((ch8*)"--OutputPID");
		if (_Params.m_bSeparateStdErr)
			ParametersList.f_Insert((ch8*)"--NoStdErr");
		else
			ParametersList.f_Insert((ch8*)"--OutputStdErrToStdOut");
	}

	ParametersList.f_Insert((ch8 *)nullptr);

	char *ToolPath = Executable.f_GetStrUniqueWritable();

	AuthorizationRef Authorization;

	auto OnExit = g_OnScopeExit / [&]()
		{
			AuthorizationFree(Authorization, kAuthorizationFlagDestroyRights);
		}
	;

	OSStatus Status = AuthorizationCreate(NULL, kAuthorizationEmptyEnvironment, kAuthorizationFlagDefaults, &Authorization);
	if (Status != errAuthorizationSuccess)
	{
		_Errors += NMib::NPlatform::fg_FormatOSStatus("AuthorizationCreate (launch elevated)", Status);
		_Errors += DMibNewLine;
		return false;
	}

	AuthorizationRights Rights = {0};
	AuthorizationItem Right;
	if (CSystem::ms_PlatformVersion >= 10'07'00)
	{
		Right = { kAuthorizationRightExecute, 0, NULL, 0 };
		Rights.count = 1;
		Rights.items = &Right;
	}

	AuthorizationFlags Flags =	kAuthorizationFlagInteractionAllowed|kAuthorizationFlagPreAuthorize|kAuthorizationFlagExtendRights;

	NStr::CStr Prompt = _Params.m_Prompt;
	NStr::CStr IconPath = _Params.m_IconPath;

	AuthorizationEnvironment Environment = {0};
	AuthorizationItem EnvironmentItems[2];
	if (CSystem::ms_PlatformVersion >= 10'07'00)
	{
		EnvironmentItems[0].name = kAuthorizationEnvironmentPrompt;
		EnvironmentItems[0].valueLength = (size_t)Prompt.f_GetLen();
		EnvironmentItems[0].value = (void*)Prompt.f_GetStrUniqueWritable();
		EnvironmentItems[0].flags = 0;

		EnvironmentItems[1].name = kAuthorizationEnvironmentIcon;
		EnvironmentItems[1].valueLength = (size_t)IconPath.f_GetLen();
		EnvironmentItems[1].value = (void*)IconPath.f_GetStrUniqueWritable();
		EnvironmentItems[1].flags = 0;

		Environment.count = 2;
		Environment.items = EnvironmentItems;
	}

	Status = AuthorizationCopyRights(Authorization, &Rights, &Environment, Flags, NULL);
	if (Status != errAuthorizationSuccess)
	{
		_Errors += NMib::NPlatform::fg_FormatOSStatus("AuthorizationCopyRights (launch elevated)", Status);
		_Errors += DMibNewLine;
		return false;
	}

	FILE *pPipe = NULL;

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
	Status = AuthorizationExecuteWithPrivileges(Authorization, ToolPath, kAuthorizationFlagDefaults, ParametersList.f_GetArray(), &pPipe);
#pragma clang diagnostic pop

	if (Status != errAuthorizationSuccess)
	{
		_Errors += NMib::NPlatform::fg_FormatOSStatus("AuthorizationExecuteWithPrivileges (launch elevated)", Status);
		return false;
	}

	close(_hStdOutRead);
	close(_hStdInWrite);

	_hStdOutRead = dup(fileno(pPipe));
	_hStdInWrite = dup(fileno(pPipe));

	fclose(pPipe);

	if (_Params.m_bStdOutPID)
	{
		char Buffer[17];
		int BytesRead = 0;

		while (BytesRead < 16)
			BytesRead += read (_hStdOutRead, Buffer + BytesRead, 16 - BytesRead);

		NMib::NStr::CStr ProcessID(Buffer, BytesRead);
		ProcessID = "0x" + ProcessID;
		o_PID.f_Store(ProcessID.f_ToInt());
	}

#ifdef F_SETNOSIGPIPE
	fcntl(_hStdOutRead, F_SETNOSIGPIPE, 1);
	fcntl(_hStdInWrite, F_SETNOSIGPIPE, 1);
#endif

	return true;
}
