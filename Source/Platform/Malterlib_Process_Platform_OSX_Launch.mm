// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Process/ProcessLaunch>

#include <Mib/Core/PlatformSpecific/OSXObjC>
#include <Mib/Core/PlatformSpecific/OSXOSStatus>

#import <ApplicationServices/ApplicationServices.h>

#include "Malterlib_Process_Platform_POSIX_PlatformSpecific.h"

DMibDeprecatedSuppressStart;

namespace NMib::NProcess::NPlatform
{
	bool fg_MacOSX_LaunchUIExecutable(NMib::NProcess::CProcessLaunchParams const& _Params, pid_t& _oPID, NMib::NStr::CStr &o_Errors)
	{
		CAutoReleasePool ARPool;

		NStr::CStr const &UTF8Target = _Params.m_Target;

		bool bReturn = true;

		ProcessSerialNumber PSN;

		LSApplicationParameters Params;

		FSRef ApplicationRef;
		Boolean bIsDirectory;
		OSStatus Result = FSPathMakeRef((const UInt8 *)UTF8Target.f_GetStr(), &ApplicationRef, &bIsDirectory);
		if (Result < 0)
		{
			o_Errors += NMib::NPlatform::fg_FormatOSStatus("FSPathMakeRef (launch UI executable)", Result);
			o_Errors += "\n";
			return false;
		}

		Params.version = 0;
		Params.flags = kLSLaunchNewInstance | kLSLaunchDontAddToRecents;
		Params.application = &ApplicationRef;
		Params.asyncLaunchRefCon = nullptr;      /* The client refCon which will appear in subsequent launch notifications */
		Params.environment = nullptr;
		NSMutableDictionary *pEnv = nullptr;

		if (!_Params.m_Environment.f_IsEmpty())
		{ // Set environment
			pEnv = [[NSMutableDictionary alloc] init];

			for (auto &EnvironmentVar : _Params.m_Environment)
			{
				[
					pEnv setObject: NMib::NPlatform::fg_MaxOSX_GetString(EnvironmentVar)
					forKey: NMib::NPlatform::fg_MaxOSX_GetString(_Params.m_Environment.fs_GetKey(EnvironmentVar))
				];
			}

			Params.environment = (__bridge CFDictionaryRef)pEnv;
		}
		NSMutableArray *pArgs = nullptr;

		{ // Set Arguments
			NContainer::TCVector<NStr::CStr> Arguments;

			{
				NStr::CStr ParamStr = _Params.m_Parameters;
				NStr::CStr CurParam;
				while(!ParamStr.f_IsEmpty())
				{
					CurParam = NStr::fg_GetStrSepEscaped<'\"'>(ParamStr, " ");
					Arguments.f_Insert(CurParam);
				}
			}

			pArgs = [NSMutableArray arrayWithCapacity:Arguments.f_GetLen()];

			for (auto &Argument : Arguments)
				[pArgs addObject: NMib::NPlatform::fg_MaxOSX_GetString(Argument)];

			Params.argv = (__bridge CFArrayRef)pArgs;
		}

		Params.initialEvent = nullptr;

		Result = LSOpenApplication(&Params, &PSN);

		if (Result >= 0)
		{
			pid_t PID;
			Result = GetProcessPID(&PSN, &PID);

			if (Result >= 0)
				_oPID = PID;
			else
			{
				o_Errors += NMib::NPlatform::fg_FormatOSStatus("GetProcessPID (launch UI executable)", Result);
				o_Errors += "\n";
			}
		}
		else
		{
			o_Errors += NMib::NPlatform::fg_FormatOSStatus("LSOpenApplication (launch UI executable)", Result);
			o_Errors += "\n";
			bReturn = false;
		}

		return bReturn;
	}

	bool fg_MacOSX_LaunchFinder(NMib::NProcess::CProcessLaunchParams const& _Params, pid_t& _oPID, NMib::NStr::CStr &o_Errors)
	{
		CAutoReleasePool ARPool;

		NStr::CStr Target = _Params.m_Target.f_Replace("select,", "");
		Target = Target.f_Replace("\"", "");

		NSString* pNSTarget = NMib::NPlatform::fg_MaxOSX_GetString(Target);

		if (CSystem::ms_PlatformVersion >= 10'06'00)
		{
			NSURL* pURL = [[NSURL alloc] initFileURLWithPath:pNSTarget];
			if (!pURL)
				return false;

			NSArray* pURLArray = [NSArray arrayWithObject:pURL];
			if (!pURLArray)
				return false;

			_oPID = 0;
			[[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:pURLArray];
		}
		else
		{
			NSString *pNSPath = NMib::NPlatform::fg_MaxOSX_GetString(NMib::NFile::CFile::fs_GetPath(Target));
			[[NSWorkspace sharedWorkspace] selectFile:pNSTarget inFileViewerRootedAtPath:pNSPath];
		}

		return true;
	}

	bool fg_MacOSX_LaunchDocument(NMib::NProcess::CProcessLaunchParams const& _Params, pid_t& _oPID, NMib::NStr::CStr &o_Errors)
	{
		NMib::CAutoReleasePool ARPool;

		NMib::NStr::CStr Document = _Params.m_Target;
		NMib::NStr::CStr Program;

		if (_Params.m_LaunchType == NMib::NProcess::EProcessLaunchType_Document && _Params.m_Target.f_Find(".app") != -1 && !_Params.m_Parameters.f_IsEmpty())
		{
			Program = _Params.m_Target;
			Document = _Params.m_Parameters;
			Document = Document.f_Replace("\"", "");
		}
		else if (_Params.m_LaunchType == NMib::NProcess::EProcessLaunchType_URL)
		{
			// Make sure the scheme is added if not already present
			bool bSchemeExist = false;
			aint Pos = Document.f_Find(":");
			if (Pos > -1)
			{
				NMib::NStr::CStr Scheme = Document.f_Extract(0, Pos);
				const ch8 * pParse = Scheme;
				bSchemeExist = true;
				while (*pParse)
				{
					// Check for allowed RFC 1738 scheme chars
					if (NMib::NStr::fg_CharIsAlphabetical(*pParse) || NMib::NStr::fg_CharIsNumber(*pParse) || *pParse == '+' || *pParse == '.' || *pParse == '-')
						++pParse;
					else
					{
						bSchemeExist = false;
						break;
					}

				}
			}

			if (!bSchemeExist)
				Document = (NMib::NStr::CStr::CFormat("http://{}") << Document).f_GetStr();
		}

		NSString* pNSTarget = NMib::NPlatform::fg_MaxOSX_GetString(Document);
		NSURL* pURL = nullptr;

		if (_Params.m_LaunchType == NMib::NProcess::EProcessLaunchType_Document)
			pURL = [[NSURL alloc] initFileURLWithPath: pNSTarget];
		else if (_Params.m_LaunchType == NMib::NProcess::EProcessLaunchType_URL)
			pURL = [[NSURL alloc] initWithString: pNSTarget];
		else
			return false;

		if (!pURL)
			return false;

		bool bReturn = true;

		NSArray *pURLArray = [NSArray arrayWithObject:pURL];

		ProcessSerialNumber PSN;
		OSStatus Result;

		if (Program.f_IsEmpty())
			Result = LSOpenURLsWithRole((__bridge CFArrayRef)pURLArray, kLSRolesAll, NULL, NULL, &PSN, 1);
		else
		{
			NSString* pPath = NMib::NPlatform::fg_MaxOSX_GetString(Program);

			FSRef AppRef;
			if (FSPathMakeRef((const UInt8*)[pPath fileSystemRepresentation], &AppRef, NULL) != noErr)
				return false;

			LSApplicationParameters AppParams = { 0, kLSLaunchDefaults, &AppRef, NULL };
			Result = LSOpenURLsWithRole((__bridge CFArrayRef)pURLArray, kLSRolesAll, NULL, &AppParams, &PSN, 1);
		}

		if (Result >= 0)
		{
			pid_t PID;
			Result = GetProcessPID(&PSN, &PID);

			if (Result >= 0)
				_oPID = PID;
			else
				o_Errors += "Failed to get launched process' PID.\n";
		}
		else
		{
			o_Errors += NMib::NPlatform::fg_FormatOSStatus("LSOpenURLsWithRole (launch document)", Result);
			o_Errors += "\n";
			bReturn = false;
		}

		return bReturn;
	}

	bool fg_MacOSX_RegisterURLHandler(NStr::CStr const &_Protocol, NStr::CStr const& _ExePath, NStr::CStr const &_Params)
	{
		if (_ExePath != NMib::NFile::CFile::fs_GetProgramPath())
			return false;

		NStr::CStr const &Exe = _ExePath;

		CFURLRef AppURL = CFURLCreateFromFileSystemRepresentation
			(
				NULL
				, (UInt8 const*)Exe.f_GetStr()
				, Exe.f_GetLen()
				, FALSE
			)
		;

		OSStatus Result;

		Result = LSRegisterURL(AppURL, false);
		if (Result < 0)
			return false;

		CFRelease(AppURL);

		NStr::CStr const &Scheme = _Protocol;

		CFStringRef CFScheme = CFStringCreateWithBytes
			(
				NULL
				, (UInt8 const*)Scheme.f_GetStr()
				, Scheme.f_GetLen()
				, kCFStringEncodingUTF8
				, FALSE
			)
		;

		auto BundleID = [[NSBundle mainBundle] bundleIdentifier];
		Result = LSSetDefaultHandlerForURLScheme(CFScheme, (__bridge CFStringRef)BundleID);

		CFRelease(CFScheme);

		return Result >= 0;
	}

	bool fg_MacOSX_DeRegisterURLHandler(NStr::CStr const &_Protocol)
	{
		NStr::CStr const &Scheme = _Protocol;

		CFStringRef SchemeRef = CFStringCreateWithBytes(NULL, (UInt8 const*)Scheme.f_GetStr(), Scheme.f_GetLen(), kCFStringEncodingUTF8, FALSE);

		auto BundleID = [[NSBundle mainBundle] bundleIdentifier];
		OSStatus Result = LSSetDefaultHandlerForURLScheme(SchemeRef, (__bridge CFStringRef)BundleID);

		CFRelease(Scheme);

		return Result >= 0;
	}


	bool fg_MacOSX_Process_RegisterAtStartup(NStr::CStr const& _ExePath, NStr::CStr const &_Params, NStr::CStr const& _Name)
	{
		if (!_Params.f_IsEmpty())
			DMibError("Registering for startup with parameters is not supported on OSX.");

		NStr::CStr ExePath = _ExePath;
		int iAppBundle = _ExePath.f_FindReverse(".app");
		if (iAppBundle == -1)
			DMibError("Registering for startup is only supported for bundles on OSX.");

		ExePath = ExePath.f_Left(iAppBundle + 4);

		CAutoReleasePool ARPool;

		LSSharedFileListRef LoginItems = LSSharedFileListCreate(NULL, kLSSharedFileListSessionLoginItems, NULL);

		UInt32 SeedValue;
		CFURLRef ThePath = NULL;

		CFArrayRef LoginItemsArray = LSSharedFileListCopySnapshot(LoginItems, &SeedValue);

		NSString *BundlePath = NMib::NPlatform::fg_MaxOSX_GetString(ExePath);
		bool bFound = false;

		for (id Item in (__bridge NSArray*)LoginItemsArray)
		{
			LSSharedFileListItemRef ItemRef = (__bridge LSSharedFileListItemRef)Item;
			if (LSSharedFileListItemResolve(ItemRef, 0, (CFURLRef*) &ThePath, NULL) == noErr)
			{
				if ([[(__bridge NSURL *)ThePath path] hasPrefix:BundlePath])
				{
					if (ThePath != NULL)
						CFRelease(ThePath);
					// Already exists.
					bFound = true;
					break;
				}

				if (ThePath != NULL)
					CFRelease(ThePath);
			}
		}

		if (LoginItemsArray != NULL)
			CFRelease(LoginItemsArray);

		auto URL = [NSURL fileURLWithPath:BundlePath];

		LSSharedFileListItemRef Item = LSSharedFileListInsertItemURL(LoginItems, kLSSharedFileListItemLast, NULL, NULL, (__bridge CFURLRef)URL, NULL, NULL);
		if (Item)
			CFRelease(Item);

		CFRelease(LoginItems);

		return true;
	}

	bool fg_MacOSX_Process_DeRegisterAtStartup(NStr::CStr const &_ExePath, NStr::CStr const &_Params, NStr::CStr const &_Name)
	{
		if (!_Params.f_IsEmpty())
		{
			DMibError("Deregistering for startup with parameters is not supported on OSX.");
		}

		NStr::CStr ExePath = _ExePath;
		int iAppBundle = _ExePath.f_FindReverse(".app");
		if (iAppBundle == -1)
			DMibError("Registering for startup is only supported for bundles on OSX.");

		ExePath = ExePath.f_Left(iAppBundle + 4);

		CAutoReleasePool ARPool;

		LSSharedFileListRef LoginItems = LSSharedFileListCreate(NULL, kLSSharedFileListSessionLoginItems, NULL);

		UInt32 SeedValue;
		CFURLRef ThePath = NULL;

		CFArrayRef LoginItemsArray = LSSharedFileListCopySnapshot(LoginItems, &SeedValue);

		NSString* BundlePath = NMib::NPlatform::fg_MaxOSX_GetString(ExePath);

		for (id Item in (__bridge NSArray *)LoginItemsArray)
		{
			LSSharedFileListItemRef ItemRef = (__bridge LSSharedFileListItemRef)Item;
			if (LSSharedFileListItemResolve(ItemRef, 0, (CFURLRef *)&ThePath, NULL) == noErr)
			{
				if ([[(__bridge NSURL *)ThePath path] hasPrefix:BundlePath])
					LSSharedFileListItemRemove(LoginItems, ItemRef);

				if (ThePath != NULL)
					CFRelease(ThePath);
			}
		}

		if (LoginItemsArray != NULL)
			CFRelease(LoginItemsArray);

		CFRelease(LoginItems);

		return true;
	}
}

DMibDeprecatedSuppressStop;
