// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Process/ProcessLaunch>

#include <Mib/Core/PlatformSpecific/MacOSObjC>

#include <CoreFoundation/CoreFoundation.h>

namespace NMib::NProcess::NPlatform
{
	namespace
	{
		NStr::CStr fg_GetDictionaryValue(CFDictionaryRef _pDict, CFStringRef _pKey, NStr::CStr const &_Default)
		{
			CFStringRef pValue = (CFStringRef)CFDictionaryGetValue(_pDict, _pKey);
			if (pValue)
				return NMib::NPlatform::fg_MacOS_GetString(pValue);

			return _Default;
		}
	}
}

void NMib::NProcess::NPlatform::fg_Process_GetVersionInfo(NMib::NStr::CStr const &_File, NMib::NProcess::CVersionInfo &_VersionInfo)
{
	NStr::CStr CanonicalFile = _File;

	_VersionInfo.m_Major = 0;
	_VersionInfo.m_Minor = 0;
	_VersionInfo.m_Revision = 0;
	_VersionInfo.m_MinorRevision = 0;
	_VersionInfo.m_Branch = NStr::CStr();

	CAutoReleasePool ARPool;

	NSString *pFileName = NMib::NPlatform::fg_MacOS_GetString(CanonicalFile);
	NSURL *pURL = [NSURL fileURLWithPath: pFileName];

	if (!pURL)
		return;

	CFDictionaryRef pPList = CFBundleCopyInfoDictionaryForURL((CFURLRef)pURL);

	auto Cleanup = g_OnScopeExit / [&]
		{
			if (pPList)
				CFRelease(pPList);
		}
	;

	if (!pPList)
	{
		do
		{
			auto Directory = NFile::CFile::fs_GetPath(CanonicalFile);
			if (NFile::CFile::fs_GetFile(Directory) != "MacOS")
				break;

			Directory = NFile::CFile::fs_GetPath(Directory);
			if (NFile::CFile::fs_GetFile(Directory) != "Contents")
				break;

			Directory = NFile::CFile::fs_GetPath(Directory);
			if (NFile::CFile::fs_GetExtension(Directory) != "app")
				break;

			pFileName = NMib::NPlatform::fg_MacOS_GetString(Directory);
			pURL = [NSURL fileURLWithPath: pFileName];
			if (pURL)
				pPList = CFBundleCopyInfoDictionaryForURL((CFURLRef)pURL);
		}
		while (false)
			;

		if (!pPList)
			return;
	}

	NStr::CStr BundleVersion = fg_GetDictionaryValue(pPList, CFSTR("CFBundleShortVersionString"), NStr::CStr::fs_ToStr(_VersionInfo.m_Major));

	NStr::CStr BundleMajor = fg_GetStrSep(BundleVersion, ".");
	NStr::CStr BundleMinor = fg_GetStrSep(BundleVersion, ".");
	NStr::CStr BundleRevision = fg_GetStrSep(BundleVersion, ".");
	if (!BundleMajor.f_IsEmpty())
		_VersionInfo.m_Major = BundleMajor.f_ToInt(uint16(0));
	if (!BundleMinor.f_IsEmpty())
		_VersionInfo.m_Minor = BundleMinor.f_ToInt(uint16(0));
	if (!BundleRevision.f_IsEmpty())
		_VersionInfo.m_Revision = BundleRevision.f_ToInt(uint16(0));

	NStr::CStr BuildTime = fg_GetDictionaryValue(pPList, CFSTR("BuildTime"), "");
	NStr::CStr BuildTimeSeconds = fg_GetStrSep(BuildTime, ":");
	NStr::CStr BuildTimeFraction = fg_GetStrSep(BuildTime, ":");

	if (!BuildTimeSeconds.f_IsEmpty())
		_VersionInfo.m_BuildTime = NMib::NTime::CTime::fs_Create(BuildTimeSeconds.f_ToInt(int64(0)), BuildTimeFraction.f_ToInt(uint64(0)));

	_VersionInfo.m_Major = fg_GetDictionaryValue(pPList, CFSTR("ProductVersionMajor"), NStr::CStr::fs_ToStr(_VersionInfo.m_Major)).f_ToInt(uint16(0));
	_VersionInfo.m_Minor = fg_GetDictionaryValue(pPList, CFSTR("ProductVersionMinor"), NStr::CStr::fs_ToStr(_VersionInfo.m_Minor)).f_ToInt(uint16(0));
	_VersionInfo.m_Revision = fg_GetDictionaryValue(pPList, CFSTR("ProductVersionRevision"), NStr::CStr::fs_ToStr(_VersionInfo.m_Revision)).f_ToInt(uint16(0));
	_VersionInfo.m_Branch = fg_GetDictionaryValue(pPList, CFSTR("MalterlibBranch"), _VersionInfo.m_Branch);
	_VersionInfo.m_GitBranch = fg_GetDictionaryValue(pPList, CFSTR("MalterlibGitBranch"), _VersionInfo.m_GitBranch);
	_VersionInfo.m_GitCommit = fg_GetDictionaryValue(pPList, CFSTR("MalterlibGitCommit"), "");
}
