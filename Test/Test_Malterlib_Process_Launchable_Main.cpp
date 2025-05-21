// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Process/ProcessLaunch>

#include "MalterlibBuild.h"

using namespace NMib;

using FOnOpenURL = void (NStr::CStr const &_URL);

#ifdef DPlatformFamily_macOS
void fg_CreateMacOSEventHandler(FOnOpenURL* _pfOnOpenURL);
void fg_DestroyMacOSEventHandler();
void fg_RunApplicationMain(int argc, char *argv[]);
#endif


void OnOpenURL(NStr::CStr const& _URL)
{
	try
	{
		//NStr::CStr Path = NFile::CFile::fs_GetProgramDirectory() + "/../../../URLOpened.txt";
//		NStr::CStr Path = "/Deploy/Malterlib/URLOpened.txt";

		// "hsXXXXXX://"
		NStr::CStr Path = _URL.f_Extract(11);

		NFile::CFile::fs_WriteStringToFile(NStr::CStr(Path), "URL Opened!\n");
	}
	catch(NFile::CExceptionFile& _Ex)
	{
		DMibConOut("ExceptionFile: {}\n", _Ex.f_GetErrorStr());
	}
}

int calling_convention_c main(int _ArgC, char ** _pArgV)
{
	DMibConOutRaw("Launchable!\n");

	NContainer::TCVector<NStr::CStr> CommandLineArgs;
	NSys::fg_Process_GetCommandLineArgs(CommandLineArgs);

	if (CommandLineArgs.f_GetLen() < 2)
	{
		return 0;
	}
	
	for (auto & Arg : CommandLineArgs)
	{
		DMibTrace("Arg: {}\n", Arg);
		if (Arg.f_CmpNoCase("-crash") == 0)
		{
			DMibPDebugBreak; // Test breakpad
			return 0;
		}
	}

	if (CommandLineArgs.f_GetLen() >= 2)
	{
		if (CommandLineArgs[1].f_CmpNoCase("-register") == 0)
		{
			DMibConOutRaw("Registering URL scheme: " DLaunchableScheme "\n");
			NProcess::CProcessLaunch::fs_RegisterURLHandler(DLaunchableScheme, NFile::CFile::fs_GetProgramPath(), "");
			return 0;
		}
		else if (CommandLineArgs[1].f_CmpNoCase("-deregister") == 0)
		{
			DMibConOutRaw("Deregistering URL scheme: " DLaunchableScheme "\n");
			NProcess::CProcessLaunch::fs_DeRegisterURLHandler(DLaunchableScheme);
			return 0;
		}
		else if (CommandLineArgs[1].f_CmpNoCase("-atstartup") == 0)
		{
			NMib::NProcess::CProcessLaunch::fs_RegisterAtStartup(NMib::NFile::CFile::fs_GetProgramPath(), "", "");
			return 0;
		}
		else if (CommandLineArgs[1].f_CmpNoCase("-notatstartup") == 0)
		{
			NMib::NProcess::CProcessLaunch::fs_DeRegisterAtStartup(NMib::NFile::CFile::fs_GetProgramPath(), "", "");
			return 0;
		}
	}
	
	CommandLineArgs.f_Clear();

#ifdef DPlatformFamily_macOS
	{
		fg_CreateMacOSEventHandler(OnOpenURL);

		fg_RunApplicationMain(_ArgC, _pArgV);

		fg_DestroyMacOSEventHandler();
	}

#endif

	return 0;
}

DMibAppNoClass;

#ifdef DPlatformFamily_Windows
int __stdcall wWinMain(struct HINSTANCE__ * hInstance, struct HINSTANCE__ * hPrevInstance, wchar_t *lpCmdLine,int nShowCmd){;} 
int __cdecl wmain(int argc, wchar_t *argv[], wchar_t *envp[]){} 
//int __cdecl main(int argc, wchar_t *argv[]){} 
int __stdcall WinMain(struct HINSTANCE__ * hInstance, struct HINSTANCE__ * hPrevInstance, char *lpCmdLine,int nShowCmd){;}
#endif
