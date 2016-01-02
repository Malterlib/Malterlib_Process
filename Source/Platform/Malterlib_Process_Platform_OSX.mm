// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Process/ProcessLaunch>

#include <Mib/Core/PlatformSpecific/OSXObjC>

#include <CoreFoundation/CoreFoundation.h>

namespace NMib
{
	namespace NProcess
	{
		namespace NPlatform
		{
			namespace
			{
				NStr::CStr fg_GetDictionaryValue(NSDictionary *_pDict, NStr::CStr const &_Key, NStr::CStr const &_Default)
				{
					NSString *pObjectForKey = [_pDict objectForKey: NMib::NPlatform::fg_MaxOSX_GetString(_Key)];
					if (pObjectForKey)
					{
						return NMib::NPlatform::fg_MaxOSX_GetString(pObjectForKey);
					}
						
					return _Default;
				}
			}
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
	
	NSURL *pURL = [NSURL fileURLWithPath: NMib::NPlatform::fg_MaxOSX_GetString(CanonicalFile)];
	if (pURL)
	{
		NSDictionary *pPList = (NSDictionary *)CFBundleCopyInfoDictionaryForURL((CFURLRef)pURL);
		
		if (pPList)
		{
			NStr::CStr BundleVersion = fg_GetDictionaryValue(pPList, "CFBundleShortVersionString", NStr::CStr::fs_ToStr(_VersionInfo.m_Major));
			
			NStr::CStr BundleMajor = fg_GetStrSep(BundleVersion, ".");
			NStr::CStr BundleMinor = fg_GetStrSep(BundleVersion, ".");
			NStr::CStr BundleRevision = fg_GetStrSep(BundleVersion, ".");					
			if (!BundleMajor.f_IsEmpty())
				_VersionInfo.m_Major = BundleMajor.f_ToInt(uint16(0));
			if (!BundleMinor.f_IsEmpty())
				_VersionInfo.m_Minor = BundleMinor.f_ToInt(uint16(0));
			if (!BundleRevision.f_IsEmpty())
				_VersionInfo.m_Revision = BundleRevision.f_ToInt(uint16(0));

			NStr::CStr BuildTime = fg_GetDictionaryValue(pPList, "BuildTime", "");
			NStr::CStr BuildTimeSeconds = fg_GetStrSep(BuildTime, ":");
			NStr::CStr BuildTimeFraction = fg_GetStrSep(BuildTime, ":");
			
			if (!BuildTimeSeconds.f_IsEmpty())
			{
				_VersionInfo.m_BuildTime = NMib::NTime::CTime::fs_Create(BuildTimeSeconds.f_ToInt(int64(0)), BuildTimeFraction.f_ToInt(uint64(0)));
			}
			
			_VersionInfo.m_Major = fg_GetDictionaryValue(pPList, "ProductVersionMajor", NStr::CStr::fs_ToStr(_VersionInfo.m_Major)).f_ToInt(uint16(0));
			_VersionInfo.m_Minor = fg_GetDictionaryValue(pPList, "ProductVersionMinor", NStr::CStr::fs_ToStr(_VersionInfo.m_Minor)).f_ToInt(uint16(0));
			_VersionInfo.m_Revision = fg_GetDictionaryValue(pPList, "ProductVersionRevision", NStr::CStr::fs_ToStr(_VersionInfo.m_Revision)).f_ToInt(uint16(0));
			_VersionInfo.m_Branch = fg_GetDictionaryValue(pPList, "MalterlibBranch", _VersionInfo.m_Branch);
			return ;
		}
	}			
}

