// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Core/PlatformSpecific/WindowsFilePath>
#include <Mib/Core/PlatformSpecific/WindowsFile>
#include <Mib/Core/PlatformSpecific/WindowsError>
#include <Mib/Core/PlatformSpecific/WindowsUndocumented>
#include <Mib/Core/PlatformSpecific/WindowsOptional>
#include <Mib/Encoding/EJson>
#include "../Malterlib_Process_Platform.h"
#include <Windows.h>
#include "Malterlib_Process_Platform_Windows.h"

#include <TlHelp32.h>
#include <Psapi.h>
#include <winternl.h>

#pragma comment(lib, "Version.lib")
namespace
{
	uint32 fg_GetPEInfo(NMib::NStr::CStr const &_File, uint16 &_MajorVersion, uint16 &_MinorVersion)
	{
		if (!NMib::NFile::CFile::fs_FileExists(_File))
			return 0;
		uint32 Timestamp = 0;
		NMib::NFile::CFile File;
		File.f_Open(_File, NMib::NFile::EFileOpen_Read | NMib::NFile::EFileOpen_ShareAll);
		uint32 Chunk[sizeof(uint32) * 256];
		CMibFilePos ToRead = File.f_GetLength() / 4;
		CMibFilePos FilePos = 0;
		uint32 EpChunk = uint32('E') << 8 | uint32('P');
		while (ToRead)
		{
			umint ThisTime = NMib::fg_Min(ToRead, 256);
			File.f_Read(Chunk, ThisTime * sizeof(uint32));
			for (umint i = 0; i < ThisTime; ++i)
			{
				if (Chunk[i] == EpChunk)
				{
					File.f_SetPosition(FilePos + i * sizeof(uint32));
					IMAGE_NT_HEADERS Headers;
					File.f_Read(&Headers, sizeof(Headers));

					if (Headers.FileHeader.Machine == IMAGE_FILE_MACHINE_I386)
					{
						File.f_SetPosition(FilePos + i * sizeof(uint32));
						IMAGE_NT_HEADERS32 Headers32;
						File.f_Read(&Headers32, sizeof(Headers32));

						_MajorVersion = Headers32.OptionalHeader.MajorImageVersion;
						_MinorVersion = Headers32.OptionalHeader.MinorImageVersion;
					}
					else if (Headers.FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64)
					{
						File.f_SetPosition(FilePos + i * sizeof(uint32));
						IMAGE_NT_HEADERS64 Headers64;
						File.f_Read(&Headers64, sizeof(Headers64));
						_MajorVersion = Headers64.OptionalHeader.MajorImageVersion;
						_MinorVersion = Headers64.OptionalHeader.MinorImageVersion;
					}

					return Headers.FileHeader.TimeDateStamp;
				}
			}
			ToRead -= ThisTime;
			FilePos += ThisTime;
		}
		return Timestamp;
	}
}

uint32 NMib::NProcess::NPlatform::fg_Win32_TranslateProcessPriority(EExecutionPriority _Priority)
{
	if (_Priority == EExecutionPriority_Default)
		return 0;
	else if (_Priority == EExecutionPriority_Highest)
		return REALTIME_PRIORITY_CLASS;
	else if (_Priority >= EExecutionPriority_High)
		return HIGH_PRIORITY_CLASS;
	else if (_Priority >= EExecutionPriority_AboveNormal)
		return ABOVE_NORMAL_PRIORITY_CLASS;
	else if (_Priority >= EExecutionPriority_Normal)
		return NORMAL_PRIORITY_CLASS;
	else if (_Priority >= EExecutionPriority_BelowNormal)
		return BELOW_NORMAL_PRIORITY_CLASS;
	else
		return IDLE_PRIORITY_CLASS;
}

void NMib::NProcess::NPlatform::fg_Process_GetVersionInfo(NMib::NStr::CStr const &_File, NMib::NProcess::CVersionInfo &_VersionInfo)
{
	NMib::NStr::CWStr File = NMib::NFile::NPlatform::fg_ConvertToWindowsPathLocal(_File);

	_VersionInfo.m_Major = 0;
	_VersionInfo.m_Minor = 0;
	_VersionInfo.m_Revision = 0;
	_VersionInfo.m_MinorRevision = 0;

	do
	{
		DWORD Size = GetFileVersionInfoSizeW(File, nullptr);
		if (Size == 0)
			break;

		NContainer::CByteVector Data;
		Data.f_SetLen(Size);
		if (!GetFileVersionInfoW(File, 0, Size, Data.f_GetArray()))
			break;

		void *pBlock = Data.f_GetArray();

		UINT QuerySize = 0;
		VS_FIXEDFILEINFO *pFixedFileInfo;

		if (!VerQueryValue(pBlock, str_utf16("\\"), (LPVOID*)&pFixedFileInfo, &QuerySize))
			break;


		_VersionInfo.m_Major = pFixedFileInfo->dwFileVersionMS >> 16;
		_VersionInfo.m_Minor = (pFixedFileInfo->dwFileVersionMS & 0xFFFF);
		_VersionInfo.m_Revision = pFixedFileInfo->dwFileVersionLS >> 16;
		_VersionInfo.m_MinorRevision = (pFixedFileInfo->dwFileVersionLS & 0xFFFF);


		struct LANGANDCODEPAGE
		{
			WORD wLanguage;
			WORD wCodePage;
		} *pTranslate;

		if (VerQueryValue(pBlock, str_utf16("\\VarFileInfo\\Translation"), (LPVOID*)&pTranslate, &QuerySize))
		{
			for (umint i = 0; i < (QuerySize/sizeof(struct LANGANDCODEPAGE)); ++i)
			{
				NStr::CWStr SubBlock = NStr::CWStr::CFormat(str_utf16("\\StringFileInfo\\{nfh,sj4,sf0,nc}{nfh,sj4,sf0,nc}\\PrivateBuild"))
					<< pTranslate[i].wLanguage
					<< pTranslate[i].wCodePage
				;

				ch16 const *pBuffer = nullptr;
				UINT BufferBytes = 0;
				if (VerQueryValue(pBlock, SubBlock.f_GetStr(), (LPVOID*)&pBuffer, &BufferBytes))
				{
					NStr::CStr BuildData = NStr::CWStr(pBuffer);
					if (BuildData.f_StartsWith("{"))
					{
						try
						{
							using namespace NEncoding;

							CEJsonSorted JsonBuildData = CEJsonSorted::fs_FromString(BuildData);

							if (auto pValue = JsonBuildData.f_GetMember("MalterlibBranch", EJsonType_String))
								_VersionInfo.m_Branch = pValue->f_String();
							if (auto pValue = JsonBuildData.f_GetMember("MalterlibGitBranch", EJsonType_String))
								_VersionInfo.m_GitBranch = pValue->f_String();
							if (auto pValue = JsonBuildData.f_GetMember("MalterlibGitCommit", EJsonType_String))
								_VersionInfo.m_GitCommit = pValue->f_String();
						}
						catch (NException::CException const &)
						{
						}
					}
					else
						_VersionInfo.m_Branch = BuildData;

					break;
				}
			}
		}
	}
	while (false)
		;

	uint16 PEMajor = 0;
	uint16 PEMinor = 0;
	uint32 Timestamp = fg_GetPEInfo(_File, PEMajor, PEMinor);
	if (PEMajor > _VersionInfo.m_Major)
	{
		_VersionInfo.m_Major = PEMajor;
		_VersionInfo.m_Minor = PEMinor / 1000;
		_VersionInfo.m_Revision = PEMinor % 1000;
	}

	_VersionInfo.m_BuildTime = NTime::CTimeConvert::fs_CreateTime(1970) + NTime::CTimeSpan((int64)Timestamp);
}

void NMib::NProcess::NPlatform::fg_Process_SetPriority(EExecutionPriority _Priority)
{
	SetPriorityClass(GetCurrentProcess(), fg_Win32_TranslateProcessPriority(_Priority));
}

NMib::EExecutionPriority NMib::NProcess::NPlatform::fg_Process_GetPriority()
{
	auto PriorityClass = GetPriorityClass(GetCurrentProcess());
	switch (PriorityClass)
	{
	case REALTIME_PRIORITY_CLASS: return EExecutionPriority_Highest;
	case HIGH_PRIORITY_CLASS: return EExecutionPriority_High;
	case ABOVE_NORMAL_PRIORITY_CLASS: return EExecutionPriority_AboveNormal;
	case NORMAL_PRIORITY_CLASS: return EExecutionPriority_Normal;
	case BELOW_NORMAL_PRIORITY_CLASS: return EExecutionPriority_BelowNormal;
	case IDLE_PRIORITY_CLASS: return EExecutionPriority_Lowest;
	default: return EExecutionPriority_Default;
	}
}

NMib::NContainer::TCVector<NMib::NProcess::CProcessInfo> NMib::NProcess::NPlatform::fg_Process_Enum(NProcess::EProcessInfoFlag _ToGet, NContainer::TCVector<NProcess::CProcessInfo> * _pOldEnum)
{
	NContainer::TCVector<NProcess::CProcessInfo> Ret;

	CProcessEntry RootProcess;
	HANDLE hProcessSnap;
	PROCESSENTRY32 ProcessEntry;

	// Take a snapshot of all processes in the system.
	hProcessSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (hProcessSnap != INVALID_HANDLE_VALUE)
	{
		ProcessEntry.dwSize = sizeof( PROCESSENTRY32 );

		if (Process32First(hProcessSnap, &ProcessEntry))
		{
			do
			{
				auto & NewProcess = Ret.f_Insert();

				NewProcess.m_ProcessID = ProcessEntry.th32ProcessID;
				if (_ToGet & EProcessInfoFlag_ParentProcessID)
					NewProcess.m_ParentProcessID = ProcessEntry.th32ParentProcessID;

				if (!(_ToGet & (EProcessInfoFlag_StartTime | EProcessInfoFlag_FileName | EProcessInfoFlag_FullPath | EProcessInfoFlag_Args | EProcessInfoFlag_User)))
					continue;

				HANDLE pThisProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, false, ProcessEntry.th32ProcessID);

				if (_ToGet & EProcessInfoFlag_StartTime)
				{
					FILETIME CreateTime;
					FILETIME ExitTime;
					FILETIME KernelTime;
					FILETIME UserTime;
					if (GetProcessTimes(pThisProcess, &CreateTime, &ExitTime, &KernelTime, &UserTime))
						NewProcess.m_StartTime = uint64(CreateTime.dwHighDateTime) << 32 | uint64(CreateTime.dwLowDateTime);
				}

				if (_ToGet & EProcessInfoFlag_User)
				{
					do
					{
						HANDLE ProcessToken;
						if (!OpenProcessToken(pThisProcess, TOKEN_READ, &ProcessToken))
							break;

						auto Cleanup = g_OnScopeExit / [&]
							{
								CloseHandle(ProcessToken);
							}
						;

						uint32 NeededLength = 0;
						GetTokenInformation(ProcessToken, TokenUser, nullptr, 0, &NeededLength);

						NContainer::CByteVector TokenData;
						if (!GetTokenInformation(ProcessToken, TokenUser, TokenData.f_GetArray(NeededLength), NeededLength, &NeededLength))
							break;

						TOKEN_USER &TokenUserInfo = *((TOKEN_USER *)TokenData.f_GetArray());

						SID_NAME_USE AccountType;
						NMib::NStr::CWStr ReferencedDomainName;
						uint32 ReferencedDomainNameSize = 8192;

						NMib::NStr::CWStr UserName;
						uint32 UserNameSize = 8192;

						if (!LookupAccountSid(nullptr, TokenUserInfo.User.Sid, UserName.f_GetStr(8192 + 1), &UserNameSize, ReferencedDomainName.f_GetStr(8192+1), &ReferencedDomainNameSize, &AccountType))
							break;

						using namespace ::NMib::NStr;
						if (ReferencedDomainName.f_IsEmpty())
							NewProcess.m_RealUID = NewProcess.m_EffectiveUID = UserName;
						else
							NewProcess.m_RealUID = NewProcess.m_EffectiveUID = "{}\\{}"_f << ReferencedDomainName << UserName;
					}
					while (false)
						;
				}

				NStr::CWStr ImageFileName;
				NStr::CWStr CommandLine;

				{
					UndocumentedPEB Peb;
					PROCESS_BASIC_INFORMATION BasicInfo;
					ULONG RetLen = 0;
					if (NLocal::g_OptionalFunctions.m_fNtQueryInformationProcess && !NLocal::g_OptionalFunctions.m_fNtQueryInformationProcess(pThisProcess, ProcessBasicInformation, &BasicInfo, sizeof(BasicInfo), &RetLen) && RetLen == sizeof(BasicInfo))
					{
						SIZE_T ReadBytes = 0;
						if (ReadProcessMemory(pThisProcess, BasicInfo.PebBaseAddress, &Peb, sizeof(Peb), &ReadBytes) && ReadBytes == sizeof(Peb))
						{
							Uncodumented_RTL_USER_PROCESS_PARAMETERS ProcessParams;
							if (ReadProcessMemory(pThisProcess, Peb.ProcessParameters, &ProcessParams, sizeof(ProcessParams), &ReadBytes) && ReadBytes == sizeof(ProcessParams))
							{
								if (_ToGet & (EProcessInfoFlag_FileName | EProcessInfoFlag_FullPath))
								{
									umint BufferStrLen = ProcessParams.ImagePathName.Length/sizeof(ch16);
									auto pBuffer = ImageFileName.f_GetStr(BufferStrLen + 1);
									if (ReadProcessMemory(pThisProcess, ProcessParams.ImagePathName.Buffer, pBuffer, ProcessParams.ImagePathName.Length, &ReadBytes) && ReadBytes == ProcessParams.ImagePathName.Length)
										pBuffer[BufferStrLen] = 0;
									else
										ImageFileName.f_Clear();
								}

								if (_ToGet & EProcessInfoFlag_Args)
								{
									umint BufferStrLen = ProcessParams.CommandLine.Length/sizeof(ch16);
									auto pBuffer = CommandLine.f_GetStr(BufferStrLen + 1);
									if (ReadProcessMemory(pThisProcess, ProcessParams.CommandLine.Buffer, pBuffer, ProcessParams.CommandLine.Length, &ReadBytes) && ReadBytes == ProcessParams.CommandLine.Length)
										pBuffer[BufferStrLen] = 0;
									else
										CommandLine.f_Clear();
								}
							}
						}
					}
				}

				NStr::CStr FullPath = NFile::NPlatform::fg_ConvertFromWindowsPath(NFile::NPlatform::fg_ConvertToLongWindowsPath<NStr::CWStr, NStr::CWStr>(ImageFileName, false));
				if (_ToGet & EProcessInfoFlag_FullPath)
					NewProcess.m_FullPath = FullPath;
				if (_ToGet & EProcessInfoFlag_FileName)
					NewProcess.m_FileName = NFile::CFile::fs_GetFile(FullPath);
				if ((_ToGet & EProcessInfoFlag_Args) && !CommandLine.f_IsEmpty())
				{
					NStr::CStr ExecutableName;
					NewProcess.m_Args = CProcessLaunchParams::fs_ParseCommandLineWindows(CommandLine, ExecutableName);
				}

				if (pThisProcess)
					CloseHandle(pThisProcess);
			}
			while (Process32Next(hProcessSnap, &ProcessEntry))
				;
		}

		CloseHandle(hProcessSnap);
	}
	return Ret;
}

NMib::NStr::CStr NMib::NProcess::NPlatform::fg_Process_GetUserName()
{
	uint32 Size = 0;
	::GetUserNameW(nullptr, &Size);

	++Size;
	NMib::NStr::CWStr Return;
	::GetUserNameW(Return.f_GetStr(Size), &Size);
	Return.f_TrimSize();
	return Return;
}

NMib::NStr::CStr NMib::NProcess::NPlatform::fg_Process_GetComputerAddress()
{
	return NMib::NProcess::NPlatform::fg_Process_GetComputerName();
}

NMib::NStr::CStr NMib::NProcess::NPlatform::fg_Process_GetComputerName()
{
	uint32 Size = 0;
	::GetComputerNameW(nullptr, &Size);

	++Size;
	NMib::NStr::CWStr Return;
	::GetComputerNameW(Return.f_GetStr(Size), &Size);
	Return.f_TrimSize();
	return Return;
}

NMib::NStr::CStr NMib::NProcess::NPlatform::fg_Process_GetHostName()
{
	NStr::CWStr Temp;
	DWORD Size = 0;
	GetComputerNameExW(ComputerNameDnsHostname, Temp.f_GetStr(1), &Size);
	GetComputerNameExW(ComputerNameDnsHostname, Temp.f_GetStr(Size), &Size);
	Temp.f_GetLen();
	return NStr::NPlatform::fg_StrFromWindows(Temp);
}

NMib::NStr::CStr NMib::NProcess::NPlatform::fg_Process_GetFullyQualiedHostName()
{
	NStr::CWStr Temp;
	DWORD Size = 0;
	GetComputerNameExW(ComputerNameDnsFullyQualified, Temp.f_GetStr(1), &Size);
	GetComputerNameExW(ComputerNameDnsFullyQualified, Temp.f_GetStr(Size), &Size);
	Temp.f_GetLen();
	return NStr::NPlatform::fg_StrFromWindows(Temp);
}

NMib::NStr::CStr NMib::NProcess::NPlatform::fg_Process_GetComputerDomain()
{
	NStr::CWStr Temp;
	DWORD Size = 0;
	GetComputerNameExW(ComputerNameDnsDomain, Temp.f_GetStr(1), &Size);
	GetComputerNameExW(ComputerNameDnsDomain, Temp.f_GetStr(Size), &Size);
	Temp.f_GetLen();

	return NStr::NPlatform::fg_StrFromWindows(Temp);
}

umint NMib::NProcess::NPlatform::fg_Process_GetCurrentUID()
{
	return GetCurrentProcessId();
}

umint NMib::NProcess::NPlatform::fg_Process_GetCurrentGroupUID()
{
	if (CSystem::ms_PlatformVersion >= 6'2'000000)
		return fg_GetPEB(fg_GetTEB())->ProcessParameters->ProcessGroupId;
	else
		return GetCurrentProcessId();
}

bool NMib::NProcess::NPlatform::fg_Process_GetProcessIsParentProcess(umint _ProcessID)
{
	// Not implemented yet
	return false;
}

void NMib::NProcess::NPlatform::fg_Process_GetMemoryCurrentStatistics(void *_pProcess, CProcessStatistics &_Stats)
{
	PROCESS_MEMORY_COUNTERS_EX MemoryInfo;
	NMemory::fg_MemClear(MemoryInfo);

	if (GetProcessMemoryInfo(_pProcess, (PROCESS_MEMORY_COUNTERS *)&MemoryInfo, sizeof(MemoryInfo)))
	{
		_Stats.m_Statistics("Working set size", CProcessStat(EProcessStatUnit_Bytes, MemoryInfo.WorkingSetSize, 1024 * 1024));
		_Stats.m_Statistics("Paged pool usage", CProcessStat(EProcessStatUnit_Bytes, MemoryInfo.QuotaPagedPoolUsage, 1024));
		_Stats.m_Statistics("Non paged pool usage", CProcessStat(EProcessStatUnit_Bytes, MemoryInfo.QuotaNonPagedPoolUsage, 1024));
		_Stats.m_Statistics("Page file usage", CProcessStat(EProcessStatUnit_Bytes, MemoryInfo.PagefileUsage, 1024 * 1024));
		_Stats.m_Statistics("Private usage", CProcessStat(EProcessStatUnit_Bytes, MemoryInfo.PrivateUsage, 1024 * 1024));
	}
}

void NMib::NProcess::NPlatform::fg_Process_GetMemoryOverallStatistics(void *_pProcess, CProcessStatistics &_Stats)
{
	PROCESS_MEMORY_COUNTERS_EX MemoryInfo;
	NMemory::fg_MemClear(MemoryInfo);

	if (GetProcessMemoryInfo(_pProcess, (PROCESS_MEMORY_COUNTERS *)&MemoryInfo, sizeof(MemoryInfo)))
	{
		_Stats.m_Statistics("Total page faults", CProcessStat(EProcessStatUnit_GeneralNumber, MemoryInfo.PageFaultCount));
		_Stats.m_Statistics("Peak working set size", CProcessStat(EProcessStatUnit_Bytes, MemoryInfo.PeakWorkingSetSize, 1024 * 1024));
		_Stats.m_Statistics("Peak paged pool usage", CProcessStat(EProcessStatUnit_Bytes, MemoryInfo.QuotaPeakPagedPoolUsage, 1024));
		_Stats.m_Statistics("Peak non paged pool usage", CProcessStat(EProcessStatUnit_Bytes, MemoryInfo.QuotaPeakNonPagedPoolUsage, 1024));
		_Stats.m_Statistics("Peak page file usage", CProcessStat(EProcessStatUnit_Bytes, MemoryInfo.PeakPagefileUsage, 1024 * 1024));
	}
}

void NMib::NProcess::NPlatform::fg_Process_GetExecutionCurrentStatistics(void *_pProcess, CProcessStatistics &_Stats)
{
	FILETIME CreateTime1;
	FILETIME ExitTime1;
	FILETIME KernelTime1;
	FILETIME UserTime1;

	if (GetProcessTimes(_pProcess, &CreateTime1, &ExitTime1, &KernelTime1, &UserTime1))
	{
		NTime::CTime CreateTime = NFile::NPlatform::fg_Win32_FileTimeToMalterlibTime(CreateTime1);
		NTime::CTimeSpan KernelTime = NFile::NPlatform::fg_Win32_FileTimeToMalterlibTimeSpan(KernelTime1);
		NTime::CTimeSpan UserTime = NFile::NPlatform::fg_Win32_FileTimeToMalterlibTimeSpan(UserTime1);
		NTime::CTimeSpan RunTime = NTime::CTime::fs_NowUTC() - CreateTime;

		fp64 KernelSeconds = KernelTime.f_GetSecondsFraction();
		fp64 UserSeconds = UserTime.f_GetSecondsFraction();
		fp64 RunSeconds = RunTime.f_GetSecondsFraction();

		_Stats.m_Statistics("CPU utilization Total", CProcessStat(EProcessStatUnit_Fraction, (KernelSeconds + UserSeconds) / RunSeconds));
		_Stats.m_Statistics("CPU utilization User", CProcessStat(EProcessStatUnit_Fraction, UserSeconds / RunSeconds));
		_Stats.m_Statistics("CPU utilization Kernel", CProcessStat(EProcessStatUnit_Fraction, KernelSeconds / RunSeconds));
	}
}

void NMib::NProcess::NPlatform::fg_Process_GetExecutionOverallStatistics(void *_pProcess, CProcessStatistics &_Stats)
{
	FILETIME CreateTime1;
	FILETIME ExitTime1;
	FILETIME KernelTime1;
	FILETIME UserTime1;

	if (GetProcessTimes(_pProcess, &CreateTime1, &ExitTime1, &KernelTime1, &UserTime1))
	{
		NTime::CTime CreateTime = NFile::NPlatform::fg_Win32_FileTimeToMalterlibTime(CreateTime1);
		NTime::CTime ExitTime = NFile::NPlatform::fg_Win32_FileTimeToMalterlibTime(ExitTime1);
		NTime::CTimeSpan KernelTime = NFile::NPlatform::fg_Win32_FileTimeToMalterlibTimeSpan(KernelTime1);
		NTime::CTimeSpan UserTime = NFile::NPlatform::fg_Win32_FileTimeToMalterlibTimeSpan(UserTime1);
		NTime::CTimeSpan RunTime = ExitTime - CreateTime;

		fp64 KernelSeconds = KernelTime.f_GetSecondsFraction();
		fp64 UserSeconds = UserTime.f_GetSecondsFraction();
		fp64 RunSeconds = RunTime.f_GetSecondsFraction();

		_Stats.m_Statistics("CPU utilization Total", CProcessStat(EProcessStatUnit_Fraction, (KernelSeconds + UserSeconds) / RunSeconds));
		_Stats.m_Statistics("CPU utilization User", CProcessStat(EProcessStatUnit_Fraction, UserSeconds / RunSeconds));
		_Stats.m_Statistics("CPU utilization Kernel", CProcessStat(EProcessStatUnit_Fraction, KernelSeconds / RunSeconds));
	}
}


uint64 NMib::NProcess::NPlatform::fg_Process_GetPhysicalMemory()
{
	MEMORYSTATUSEX MemoryStatus;
	MemoryStatus.dwLength = sizeof(MemoryStatus);
	GlobalMemoryStatusEx(&MemoryStatus);
	uint64 Max = MemoryStatus.ullTotalPhys;
	return Max;
}

NMib::NStr::CStr NMib::NProcess::NPlatform::fg_Process_GetOperatingSystemTag(int32 _MajorMax, int32 _MinorMax)
{
	return "Win32";
}

#pragma warning(disable:4996)

NMib::NStr::CStr NMib::NProcess::NPlatform::fg_Process_GetOperatingSystemDescription()
{
	OSVERSIONINFOEXW VersionInfo;
	NMemory::fg_MemClear(VersionInfo);
	VersionInfo.dwOSVersionInfoSize = sizeof(VersionInfo);
	if (!GetVersionExW((OSVERSIONINFO *)&VersionInfo))
		return "";

	if (VersionInfo.dwMajorVersion == 6)
	{
		if (VersionInfo.wProductType == VER_NT_WORKSTATION)
		{
			if (VersionInfo.dwMinorVersion == 0)
				return "Windows Vista";
			else if (VersionInfo.dwMinorVersion == 1)
				return "Windows 7";
			else if (VersionInfo.dwMinorVersion == 2)
				return "Windows 8";
			else
				return "Window 8"; // For anything in the future assume Windows 8 compatibility.
		}
		else if (VersionInfo.wProductType == VER_NT_SERVER)
		{ // OS is Windows Server 2008 R2, Windows Server 2008, Windows Server 2003, or Windows 2000 Server.
			if (VersionInfo.dwMinorVersion == 1)
				return "Windows Server 2008 R2";
			else if (VersionInfo.dwMinorVersion >= 2)
				return "Windows Server 2012";
			else
				return "Windows Server 2008";
		}
		else if (VersionInfo.wProductType == VER_NT_DOMAIN_CONTROLLER)
		{ // OS is Windows Server 2008 R2, Windows Server 2008, Windows Server 2003, or Windows 2000 Server.
			return "Windows Vista";
		}
		else
			return  "Windows Unknown";
	}
	if (VersionInfo.dwMajorVersion == 5)
	{
		if (VersionInfo.dwMinorVersion == 0)
			return "Windows 2000";
		else if (VersionInfo.dwMinorVersion == 1)
			return "Windows XP";
		else if (VersionInfo.dwMinorVersion == 2)
		{
			SYSTEM_INFO SystemInfo;
			NMemory::fg_MemClear(SystemInfo);
			GetNativeSystemInfo(&SystemInfo);

			if (GetSystemMetrics(SM_SERVERR2) != 0)
				return "Windows Server 2003 R2";
			else if (VersionInfo.wSuiteMask & VER_SUITE_WH_SERVER)
				return "Windows Home Server";
			else if (GetSystemMetrics(SM_SERVERR2) == 0)
				return "Windows Server 2003";
			else if ((VersionInfo.wProductType == VER_NT_WORKSTATION) && (SystemInfo.wProcessorArchitecture==PROCESSOR_ARCHITECTURE_AMD64))
				return "Windows XP Professional x64 Edition";
			else
				return  "Windows Unknown";
		}
		else
			return  "Windows Unknown";

	}
	else
	{
		return "Windows";
	}

	DMibSafeCheck(false, "This is not implemented on Windows yet");
	return "Windows";
}

void *NMib::NProcess::NPlatform::fg_Process_Pause(umint _ProcessID)
{
	DMibError("fg_Process_Pause not implemented");
	return nullptr;
}

void NMib::NProcess::NPlatform::fg_Process_Resume(umint _ProcessID, void *_pPauseToken)
{
	DMibError("fg_Process_Resume not implemented");
}

void NMib::NProcess::NPlatform::fg_Process_Terminate(umint _ProcessID)
{
	HANDLE hProcess = OpenProcess(PROCESS_TERMINATE, false, _ProcessID);
	if (!hProcess)
		DMibError(fg_Format("When terminating process Windows returned an error from OpenProcess: {}", NMib::NPlatform::fg_Win32_GetLastErrorStr()));

	auto Cleanup = NMib::g_OnScopeExit / [&]
		{
			CloseHandle(hProcess);
		}
	;

	if (!TerminateProcess(hProcess, 255))
		DMibError(fg_Format("When terminating process Windows returned an error from TerminateProcess: {}", NMib::NPlatform::fg_Win32_GetLastErrorStr()));
}

void NMib::NProcess::NPlatform::fg_Process_Stop(umint _ProcessID)
{
	if (!GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, _ProcessID))
	{
		DMibError(fg_Format("Windows returned an error from GenerateConsoleCtrlEvent: {}", NMib::NPlatform::fg_Win32_GetLastErrorStr()));
		DMibTrace("GenerateConsoleCtrlEvent: {}{\n}", NMib::NPlatform::fg_Win32_GetLastErrorStr());
	}
}

constinit static NMib::NStorage::TCAggregate<NMib::NThread::CEvent> g_TerminationEvent = {DAggregateInit};

void NMib::NProcess::NPlatform::fg_Process_AbortWaitForTermination()
{
	g_TerminationEvent->f_SetSignaled();
}

void NMib::NProcess::NPlatform::fg_Process_WaitForTermination()
{
	PHANDLER_ROUTINE fHandler = [](DWORD _CtrlType) -> BOOL
		{
			if (_CtrlType == CTRL_C_EVENT || _CtrlType == CTRL_BREAK_EVENT)
			{
				g_TerminationEvent->f_SetSignaled();
				return true;
			}
			return false;
		}
	;

	SetConsoleCtrlHandler
		(
			fHandler
			, true
		)
	;

	g_TerminationEvent->f_Wait();

	SetConsoleCtrlHandler
		(
			fHandler
			, false
		)
	;

	g_TerminationEvent.f_Destruct();
}

constinit static NMib::NStorage::TCAggregate<NMib::NFunction::TCFunction<void ()>> gs_TerminationFunction = {DAggregateInit};

NMib::COnScopeExitShared NMib::NProcess::NPlatform::fg_Process_WaitForTermination(NFunction::TCFunction<void ()> &&_fOnTerminate)
{
	if (gs_TerminationFunction.f_IsConstructed())
		DMibError("You can only install one wait for termination handler");

	gs_TerminationFunction.f_Construct(fg_Move(_fOnTerminate));

	static PHANDLER_ROUTINE fHandler = [](DWORD _CtrlType) -> BOOL
		{
			if (_CtrlType == CTRL_C_EVENT || _CtrlType == CTRL_BREAK_EVENT)
			{
				(*gs_TerminationFunction)();
				return true;
			}

			return false;
		}
	;

	SetConsoleCtrlHandler
		(
			fHandler
			, true
		)
	;


	return g_OnScopeExitShared / []
		{
			SetConsoleCtrlHandler
				(
					fHandler
					, false
				)
			;
			gs_TerminationFunction.f_Destruct();
		}
	;
}

umint NMib::NProcess::NPlatform::fg_Process_GetMaxFilesPerProc()
{
	return 0;
}
