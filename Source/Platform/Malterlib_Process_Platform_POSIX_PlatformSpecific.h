// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include "Malterlib_Process_Platform_POSIX_Launch.h"

extern "C"
{
	#include <sys/types.h>
}
namespace NMib::NProcess::NPlatform
{
#ifdef DPlatformFamily_macOS
	bool fg_MacOS_LaunchDocument(CProcessLaunchParams const &_Params, NAtomic::TCAtomic<pid_t> &o_PID, NStr::CStr &_Errors);
	bool fg_MacOS_LaunchFinder(CProcessLaunchParams const &_Params, NAtomic::TCAtomic<pid_t> &o_PID, NStr::CStr &_Errors);
	bool fg_MacOS_LaunchUIExecutable(CProcessLaunchParams const &_Params, NAtomic::TCAtomic<pid_t> &o_PID, NStr::CStr &_Errors);
	bool fg_MacOS_LaunchExecutableWithRoot(CProcessLaunchParams const &_Params, NAtomic::TCAtomic<pid_t> &o_PID, int &_hStdOutRead, int &_hStdInWrite, NStr::CStr &_Errors);
	bool fg_MacOS_RegisterURLHandler(NStr::CStr const &_Protocol, NStr::CStr const &_ExePath, NStr::CStr const &_Params);
	bool fg_MacOS_DeRegisterURLHandler(NStr::CStr const &_Protocol);
	bool fg_MacOS_Process_RegisterAtStartup(NStr::CStr const &_ExePath, NStr::CStr const &_Params, NStr::CStr const &_Name);
	bool fg_MacOS_Process_DeRegisterAtStartup(NStr::CStr const &_ExePath, NStr::CStr const &_Params, NStr::CStr const &_Name);
	bool fg_MacOS_Process_TerminateTree(pid_t _ProcessID, NStr::CStr &_Errors);
#endif

#ifdef DPlatformFamily_Linux
	bool fg_Linux_LaunchDocumentOrURL(CProcessLaunchParams &_Params, NStr::CStr &_Program, NStr::CStr &_oErrors);
	bool fg_Linux_LaunchExecutableWithRoot(CPOSIXLaunchContext *_pLaunchContext, CProcessLaunchParams &_Params, NStr::CStr &_Program, int &_StdErrRead, int &_StdErrWrite, int &_StdInRead, int &_StdInWrite, NStr::CStr &_Errors);
	bool fg_Linux_Process_RegisterAtStartup(NStr::CStr const &_ExePath, NStr::CStr const &_Params, NStr::CStr const &_Name);
	bool fg_Linux_Process_DeRegisterAtStartup(NStr::CStr const &_ExePath, NStr::CStr const &_Params, NStr::CStr const &_Name);
	bool fg_Linux_Process_TerminateTree(pid_t _ProcessID, NStr::CStr &_Errors);
	bool fg_Linux_RegisterURLHandler(NStr::CStr const &_Protocol, NStr::CStr const &_ExePath, NStr::CStr const &_Params);
	bool fg_Linux_DeRegisterURLHandler(NStr::CStr const &_Protocol);
#endif
}
