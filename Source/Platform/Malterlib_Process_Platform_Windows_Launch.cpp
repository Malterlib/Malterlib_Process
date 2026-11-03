// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Core/Core>
#include <Mib/Process/ProcessLaunch>
#include <Mib/Core/PlatformSpecific/WindowsOptional>
#include <Mib/Core/PlatformSpecific/WindowsRegistry>
#include <Mib/Core/PlatformSpecific/WindowsError>
#include <Mib/Core/PlatformSpecific/WindowsFilePath>
#include <Mib/Core/PlatformSpecific/WindowsFile>
#include <Mib/Core/PlatformSpecific/WindowsInject>
#include <Mib/Core/PlatformSpecific/Windows>
#include <Mib/Cryptography/RandomID>

#include <Mib/Process/Platform>
#include "Malterlib_Process_Platform_Windows_Launch.h"
#include "Malterlib_Process_Platform_Windows.h"

#include <Tlhelp32.h>
#include <wtsapi32.h>
#pragma comment(lib, "wtsapi32.lib")
#include <Psapi.h>

#include <AclAPI.h>

#include <Userenv.h>
#pragma comment(lib, "Userenv.lib")

namespace NMib::NProcess::NPlatform
{
	BOOL fg_ChangeAceToWindowStation(HWINSTA _WindowStation, PSID _pSid, bool _bRemove, bool &_bAdded);
	BOOL fg_ChangeAceToDesktop(HDESK _Desktop, PSID _pSid, bool _bRemove, bool &_bAdded);

	struct CProcessLaunchLink
	{
		DMibListLinkDS_Link(CProcessLaunchLink, m_Link);
	};

	struct CSubSystem_Process_Platform_Windows_Launch : public CSubSystem
	{
		align_cacheline NThread::CMutual m_LaunchesLock;
		DMibListLinkDS_List(CProcessLaunchLink, m_Link) m_Launches;

		align_cacheline NThread::CMutual m_UserLaunchLock;

		~CSubSystem_Process_Platform_Windows_Launch()
		{
			DMibLock(m_LaunchesLock);
			DMibFastCheck(m_Launches.f_IsEmpty()); // Process launches are still active
		}

		void f_DestroyThreadSpecific() override
		{
			fg_ProcessLaunch_CancelAll();
		}
	};

	constinit TCSubSystem<CSubSystem_Process_Platform_Windows_Launch, ESubSystemDestruction_BeforeMemoryManager> g_SubSystem_Process_Platform_Windows_Launch = {DAggregateInit};

	CProcessEntry::~CProcessEntry()
	{
		m_AllProcess.f_DeleteAllDefiniteType();
	}

	bool CProcessEntry::operator == (uint32 _Process) const noexcept
	{
		return m_Process == _Process;
	}

	void CProcessEntry::f_MapProcess(uint32 _ID, uint32 _ParentID, NStr::CWStr const &_FileName, NTime::CTime const &_CreationTime)
	{
		CProcessEntry *pEntry = m_AllProcess.f_Find(_ID);

		if (!pEntry)
		{
			pEntry = fg_ConstructObject<CProcessEntry>(NMemory::CDefaultAllocator());
			pEntry->m_Process = _ID;
			m_AllProcess.f_Insert(pEntry);
		}
		pEntry->m_FileName = _FileName;
		pEntry->m_CreationTime = _CreationTime;

		CProcessEntry *pParent = m_AllProcess.f_Find(_ParentID);

		if (!pParent)
		{
			pParent = fg_ConstructObject<CProcessEntry>(NMemory::CDefaultAllocator());
			pParent->m_Process = _ParentID;
			m_AllProcess.f_Insert(pParent);
		}

		pParent->m_Children.f_Insert(pEntry);
	}

	void CProcessEntry::f_KillTree(NStr::CStr &_Log, aint _Depth)
	{
		HANDLE Process = OpenProcess(PROCESS_TERMINATE, false, m_Process);
		if (Process)
		{
			_Log += NStr::CStr(NStr::CStr::CFormat("{sf ,sj*}Killed process id {}: {}" DMibNewLine) << "" << _Depth * 3 << m_Process << m_FileName);
			TerminateProcess(Process, 255);
			CloseHandle(Process);
		}
		auto Iter = m_Children.f_GetIterator();
		while (Iter)
		{
			Iter->f_KillTree(_Log, _Depth + 1);

			++Iter;
		}
	}

	NStr::CStr fg_GetSandboxDir(NStr::CStr &_Errors)
	{
		NStr::CStr Folder = fg_GetSys()->f_GetEnvironmentVariable("MalterlibSandboxDeviceFolder");

		if (Folder.f_IsEmpty() || !NFile::CFile::fs_FileExists(Folder, NFile::EFileAttrib_Directory))
		{
			NStr::CStr SandboxFullPath = NFile::CFile::fs_GetPath(NFile::CFile::fs_GetModulePath(&fg_GetSandboxDir)) + "/MalterlibSandbox_x64.exe";

			if (!NFile::CFile::fs_FileExists(SandboxFullPath))
			{
				NMib::NPlatform::CWin32_Registry Registry(NMib::NPlatform::CWin32_Registry::ERegRoot_Win64_CurrentUser);
				if (Registry.f_ValueExists("Software\\Malterlib", "SandboxPath"))
					SandboxFullPath = Registry.f_Read_Str("Software\\Malterlib", "SandboxPath") + "/MalterlibSandbox_x64.exe";
				if (SandboxFullPath.f_IsEmpty() || !NFile::CFile::fs_FileExists(SandboxFullPath))
				{
					NMib::NPlatform::CWin32_Registry Registry(NMib::NPlatform::CWin32_Registry::ERegRoot_Win64_LocalMachine);
					if (Registry.f_ValueExists("Software\\Malterlib", "SandboxPath"))
						SandboxFullPath = Registry.f_Read_Str("Software\\Malterlib", "SandboxPath") + "/MalterlibSandbox_x64.exe";
					if (SandboxFullPath.f_IsEmpty() || !NFile::CFile::fs_FileExists(SandboxFullPath))
					{
						_Errors += "Sandbox directory not found" DMibNewLine;
						return NStr::CStr();
					}
				}
			}
			return NFile::CFile::fs_GetPath(SandboxFullPath);
		}

		return Folder;
	}

	namespace
	{
		NMib::NProcess::EProcessElevation fg_Process_GetElevation(void *_pProcess);

		constinit NAtomic::TCAtomic<uint32> g_PipeSerialNumber{0};

		uint32 fg_GetActiveSessionID()
		{
			uint32 ProcessID = GetCurrentProcessId();

			uint32 SessionID = 0xFFFFffff;
			ProcessIdToSessionId(ProcessID, &SessionID);

			if (SessionID != 0xFFFFffff)
				return SessionID;

			/*
			if (NLocal::g_OptionalFunctions.m_fWTSGetActiveConsoleSessionId)
				return NLocal::g_OptionalFunctions.m_fWTSGetActiveConsoleSessionId();*/

			return 0;
		}

		NStr::CStr fg_GetProcessName(void *_pProcess)
		{
			NStr::CWStr NameW;
			if (GetProcessImageFileName(_pProcess, NameW.f_GetStr(1024), 1024))
				return NMib::NFile::CFile::fs_GetFile(NFile::NPlatform::fg_ConvertFromWindowsPath(NameW));

			return NStr::CStr();
		}

		template <typename tf_CStrType>
		tf_CStrType fg_RemoveEscapeWinPath(tf_CStrType &_Str)
		{
			tf_CStrType Ret;
			typename tf_CStrType::CChar const *pParse = _Str;
			int Mode = 0;
			while (*pParse)
			{
				if (Mode == 0)
				{
					if (*pParse == '"')
					{
						Mode = 1;
						++pParse;
						continue;
					}
				}
				else if (Mode == 1)
				{
					if (*pParse == '"')
					{
						Mode = 0;
						++pParse;
						continue;
					}
				}
				Ret.f_AddChar(*pParse);
				++pParse;
			}
			return Ret;
		}

		template <typename tf_CStrType>
		tf_CStrType fg_GetWinPathSepEscaped(tf_CStrType &_Str, ch8 const *_pSep)
		{
			tf_CStrType Ret;
			typename tf_CStrType::CChar const *pParse = _Str;
			int32 Pos = 0;
			if (pParse[Pos] == '"')
			{
				Pos++;
				while (pParse[Pos])
				{
					if (pParse[Pos] == '"')
						break;
					Pos++;
				}
			}

			aint iFind = NStr::fg_StrFind(pParse + Pos, _pSep);
			if (iFind >= 0)
			{
				Ret = _Str.f_Left(iFind + Pos);
				_Str = _Str.f_Extract(iFind + 1 + Pos);
			}
			else
			{
				Ret = _Str;
				_Str = "";
			}
			Ret = Ret.f_TrimLeft();
			Ret = Ret.f_TrimRight();
			if (Ret[0] == '"')
				Ret = fg_RemoveEscapeWinPath(Ret);
			return Ret;
		}

		NStr::CStr fg_ExpandEnvironmentVars(NStr::CStr const &_Path)
		{
			NStr::CStr Path = _Path.f_ReplaceChar('\\', '/');
			NStr::CStr RetPath;
			while (!Path.f_IsEmpty())
			{
				NStr::CStr SubPath = fg_GetStrSep(Path, "/");
				if (SubPath.f_IsEmpty())
					RetPath += "/";
				else
				{
					if (SubPath[0] == '%')
						SubPath = fg_GetSys()->f_GetEnvironmentVariable(SubPath.f_Replace("%", ""));
					if (!RetPath.f_IsEmpty() && RetPath[RetPath.f_GetLen() - 1] == '/')
						RetPath += SubPath;
					else
						fg_AddStrSep(RetPath, SubPath, "/");
				}
			}
			return RetPath;
		}

		void fg_TerminateProcessTree(HANDLE _Process, NStr::CStr &_Log)
		{
			_Log += "\r\n\r\nProcess timed out and was killed along with any child processes:" DMibNewLine;
			{
				uint32 ProcessIDToKill = GetProcessId(_Process);
				CProcessEntry RootProcess;
				HANDLE ProcessSnapshot;
				PROCESSENTRY32 ProcessEntry;

				// Take a snapshot of all processes in the system.
				ProcessSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
				if (ProcessSnapshot != INVALID_HANDLE_VALUE)
				{
					ProcessEntry.dwSize = sizeof(PROCESSENTRY32);

					if (Process32First(ProcessSnapshot, &ProcessEntry))
					{
						do
						{
							uint32 ParentProcess = uint32(-1);
							HANDLE pParentProcess = OpenProcess(PROCESS_QUERY_INFORMATION, false, ProcessEntry.th32ParentProcessID);
							HANDLE pThisProcess = OpenProcess(PROCESS_QUERY_INFORMATION, false, ProcessEntry.th32ProcessID);

							FILETIME CreateTime1;
							FILETIME ExitTime1;
							FILETIME KernelTime1;
							FILETIME UserTime1;

							NTime::CTime CreationTime;

							if (GetProcessTimes(pThisProcess, &CreateTime1, &ExitTime1, &KernelTime1, &UserTime1))
								CreationTime = NFile::NPlatform::fg_Win32_FileTimeToMalterlibTime(CreateTime1);

							FILETIME CreateTime;
							FILETIME ExitTime;
							FILETIME KernelTime;
							FILETIME UserTime;
							if (GetProcessTimes(pParentProcess, &CreateTime, &ExitTime, &KernelTime, &UserTime))
							{
								if ((uint64 &)CreateTime1 >= (uint64 &)CreateTime)
									ParentProcess = ProcessEntry.th32ParentProcessID;
							}

							if (pParentProcess)
								CloseHandle(pParentProcess);
							if (pThisProcess)
								CloseHandle(pThisProcess);

							RootProcess.f_MapProcess(ProcessEntry.th32ProcessID, ParentProcess, ProcessEntry.szExeFile, CreationTime);
						}
						while (Process32Next(ProcessSnapshot, &ProcessEntry));
					}

					CloseHandle(ProcessSnapshot);
				}

				//RootProcess.f_TraceTree(_Log);

				CProcessEntry *pToKill = RootProcess.m_AllProcess.f_Find(ProcessIDToKill);
				if (pToKill)
					pToKill->f_KillTree(_Log);
			}
		}

		struct CConsoleRedirector : protected NThread::CThread, public CProcessLaunchLink
		{
			CConsoleRedirector();
			virtual ~CConsoleRedirector();

			NStr::CStr f_GetThreadName() override;
			aint f_Main() override;

			NStorage::CIntrusiveRefCount m_RefCount;

			DIfRefCountDebugging(NStorage::CRefCountDebugReference m_DebugSelfRef);
			DIfRefCountDebugging(NStorage::CRefCountDebugReference m_DebugSelfThreadRef);

		private:
			HANDLE mp_Event; // Win32 auto-reset event; waited together with the process handle

			NTime::CStopwatch mp_TimeSinceStart;

			NThread::CMutual mp_ExitTimeLock;
			fp64 mp_ExitTime;

			static void WINAPI fsp_StdOutReadFinished(DWORD _ErrorCode, DWORD _nBytesTransferred, LPOVERLAPPED _pOverlapped);
			static void WINAPI fsp_StdErrReadFinished(DWORD _ErrorCode, DWORD _nBytesTransferred, LPOVERLAPPED _pOverlapped);

		protected:
			struct COverallExecution
			{
				FILETIME m_CreateTime = {0};
				FILETIME m_ExitTime = {0};
				FILETIME m_KernelTime = {0};
				FILETIME m_UserTime = {0};
			};

			NThread::CMutual mp_PipeLock;
			NThread::CMutual mp_ChildProcessLock; // Closing mp_ChildProcess; f_TerminateTree cannot wait for mp_PipeLock, which a blocked stdin write holds

			HANDLE mp_StdinWrite; // write end of child's stdin pipe
			HANDLE mp_StdoutRead; // read end of child's stdout pipe
			HANDLE mp_StderrRead; // read end of child's stderr pipe
			HANDLE mp_ChildProcess;

			HANDLE mp_SandboxJob;
			HANDLE mp_ProcessGroupJob; // Holds the processes of a launch with its own process group, also after the launched process has exited

			OVERLAPPED mp_StdOutRead;
			NContainer::CByteVector mp_StdOutReadBuffer;
			OVERLAPPED mp_StdErrRead;
			NContainer::CByteVector mp_StdErrReadBuffer;

			NMib::NProcess::CProcessLaunchParams mp_LastLaunchOptions;

			NContainer::TCVector<COnScopeExitShared> mp_CleanupLoadedProfiles;

			uint32 mp_ProcessID;
			NThread::CMutual mp_NeedTerminationLock;
			NMib::NProcess::EProcessLaunchCloseFlag mp_NeedTermination;
			bool mp_bNeedWait;
			bool mp_bStarted;

			NThread::CMutual mp_OverallLock;

			NStorage::TCOptional<PROCESS_MEMORY_COUNTERS_EX> mp_OverallStats_Memory;
			NStorage::TCOptional<COverallExecution> mp_OverallStats_Execution;

			void fp_UpdateOverall();

			bool fp_LaunchChild(HANDLE _StdOut, HANDLE _StdIn, HANDLE _StdErr, NStr::CStr &_Errors);
			int fp_RedirectStdout();
			int fp_RedirectStderr();
			void fp_DestroyHandle(HANDLE &_Handle);
			void fp_ClearSandbox();
			void fp_TerminateSandbox(NStr::CStr &_Output);
			void fp_Close();
			bool fp_DoStart(NStr::CStr &_Errors);

		protected:
			void fp_OnLaunched(NMib::NStr::CStr const &_Error, void *_pProcess, bool _bSuccess);
			void fp_OnOutput(NMib::NProcess::EProcessLaunchOutputType _OutputType, NMib::NStr::CStr const &_Output);
			void fp_OnStdOutput(NMib::NProcess::EProcessLaunchOutputType _OutputType, uint8 const *_pData, umint _Size);
			void fp_OnExit(uint32 _ExitCode);
			virtual bool f_DestroyThread() override;

		public:
			bool f_Open(NMib::NProcess::CProcessLaunchParams const &_Options);
			bool f_StartRedirector(EProcessLaunchCloseFlag _Flags);
			void f_Close(NMib::NProcess::EProcessLaunchCloseFlag _Flags);
			void f_Cancel();
			fp64 f_GetRunningTime();
			bool f_IsRunning();
			bool f_SendText(NStr::CStrIO const &_Data);
			void f_CloseStdIn();
			void f_SendBinary(NContainer::CIOByteVector const &_Data);
			void f_StopProcess();
			void f_TerminateTree(NStr::CStr &o_Log);
			umint f_GetID() const;
			NMib::NProcess::CProcessStatistics f_GetOverallExecutionStatistics();
			NMib::NProcess::CProcessStatistics f_GetOverallMemoryStatistics();

			HANDLE f_GetChildProcess() const
			{
				return mp_ChildProcess;
			}

		private:
			friend struct CMultiProgramStarter;
		};

#if 0
#	define DEnableSandboxLock
#endif

		/***************************************************************************************************\
		|¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯|
		| Console redirector																				|
		|___________________________________________________________________________________________________|
		\***************************************************************************************************/

		CConsoleRedirector::CConsoleRedirector()
			: mp_Event(CreateEventA(nullptr, false, false, nullptr))
			, mp_StdinWrite(nullptr)
			, mp_bStarted(false)
			, mp_NeedTermination(NMib::NProcess::EProcessLaunchCloseFlag_None)
			, mp_bNeedWait(false)
			, mp_StdoutRead(nullptr)
			, mp_StderrRead(nullptr)
			, mp_ChildProcess(nullptr)
			, mp_SandboxJob(nullptr)
			, mp_ProcessGroupJob(nullptr)
			, mp_ProcessID(0)
			, mp_ExitTime(-1.0)
		{
			mp_TimeSinceStart.f_Start();

			auto &SubSystem = *g_SubSystem_Process_Platform_Windows_Launch;
			{
				DMibLock(SubSystem.m_LaunchesLock);
				SubSystem.m_Launches.f_Insert(this);
			}
		}

		void CConsoleRedirector::fp_TerminateSandbox(NStr::CStr &_Output)
		{
			if (mp_SandboxJob)
			{
				if (!TerminateJobObject(mp_SandboxJob, 255))
					_Output += NStr::CStr::CFormat("TerminateJobObject failed with: {}" DMibNewLine) << NMib::NPlatform::fg_Win32_GetLastErrorStr(GetLastError());
				CloseHandle(mp_SandboxJob);
				mp_SandboxJob = nullptr;
			}
		}

		// The job holds the processes whose parent has exited, and the processes that broke away from it are still found
		// through their parents
		void CConsoleRedirector::f_TerminateTree(NStr::CStr &o_Log)
		{
			{
				DMibLock(mp_ChildProcessLock);
				if (mp_ChildProcess)
					fg_TerminateProcessTree(mp_ChildProcess, o_Log);
			}

			if (mp_ProcessGroupJob && !TerminateJobObject(mp_ProcessGroupJob, 255))
				o_Log += NStr::CStr::CFormat("TerminateJobObject failed with: {}" DMibNewLine) << NMib::NPlatform::fg_Win32_GetLastErrorStr(GetLastError());
		}

		void CConsoleRedirector::fp_ClearSandbox()
		{
			NStr::CStr Errors;
			fp_TerminateSandbox(Errors);
		}

		CConsoleRedirector::~CConsoleRedirector()
		{
			fp_Close();
			fp_DestroyHandle(mp_ProcessGroupJob);

			if (mp_Event)
			{
				::CloseHandle(mp_Event);
				mp_Event = nullptr;
			}

			auto &SubSystem = *g_SubSystem_Process_Platform_Windows_Launch;
			{
				DMibLock(SubSystem.m_LaunchesLock);
				SubSystem.m_Launches.f_Remove(this);
			}
		}

		NStr::CStr CConsoleRedirector::f_GetThreadName()
		{
			return "CConsoleRedirector";
		}

		NStr::CWStr fg_FindExecutableWindows(NStr::CStr const &_Path, bool _bAllowLocate, NStr::CStr const &_LocalPaths)
		{
			auto fIsAbsolute = [](NStr::CStr const &_Candidate)
				{
					// A rooted Windows path without a drive still depends on the current drive.
					return NFile::CFile::fs_IsPathAbsolute(_Candidate) && !NFile::CFile::fs_GetDrive(_Candidate).f_IsEmpty();
				}
			;

			bool bExplicitPath = _Path.f_FindChars("/\\:") >= 0;
			NStr::CWStr WindowsPath;
			NStr::CStr FullPath;
			if (bExplicitPath)
			{
				WindowsPath = NFile::NPlatform::fg_ConvertToWindowsPath(_Path, true);
				FullPath = NFile::NPlatform::fg_ConvertFromWindowsPath(WindowsPath);
				if (!fIsAbsolute(FullPath))
					return {};
				if (NFile::CFile::fs_FileExists(FullPath, NFile::EFileAttrib_File))
					return WindowsPath;
			}

			if (!_bAllowLocate)
				return WindowsPath;
			if (!bExplicitPath)
				WindowsPath = NFile::NPlatform::fg_ConvertToWindowsPath(_Path, false);

			NStr::CWStr SearchDirectories;
			NContainer::TCVector<NStr::CWStr> Directories;
			auto fAddDirectory = [&](NStr::CStr const &_Directory)
				{
					if (!fIsAbsolute(_Directory))
						return;

					NStr::CWStr NativeDirectory = NFile::NPlatform::fg_ConvertToWindowsPath(_Directory, false, MAX_PATH, false);
					if (!NativeDirectory.f_EndsWith("\\"))
						NativeDirectory += "\\";
					if (!SearchDirectories.f_IsEmpty())
						SearchDirectories += ";";
					SearchDirectories += NativeDirectory;
					Directories.f_Insert(fg_Move(NativeDirectory));
				}
			;

			if (bExplicitPath)
				fAddDirectory(NFile::CFile::fs_GetPath(FullPath) + "/");
			else
			{
				for (umint iSource = 0; iSource < 2; ++iSource)
				{
					NStr::CStr Paths = iSource == 0 ? _LocalPaths : fg_GetSys()->f_GetEnvironmentVariable("PATH");
					for (auto const &Directory : Paths.f_Split<true>(";"))
						fAddDirectory(Directory);
				}
			}

			if (SearchDirectories.f_IsEmpty())
				return {};

			auto fSearch = [&](wchar_t const *_pExtension) -> NStr::CWStr
				{
					NStr::CWStr Result;
					DWORD nCapacity = MAX_PATH + 1;
					for (;;)
					{
						DWORD nRequired = SearchPathW(SearchDirectories, WindowsPath, _pExtension, nCapacity, Result.f_GetStr(nCapacity), nullptr);
						if (!nRequired)
							return {};
						if (nRequired < nCapacity)
							return Result;

						nCapacity = nRequired;
					}
				}
			;

			if (!NFile::CFile::fs_GetExtension(_Path).f_IsEmpty())
				return fSearch(nullptr);

			auto Com = fSearch(L".com");
			auto Exe = fSearch(L".exe");
			if (Com.f_IsEmpty())
				return Exe;
			if (Exe.f_IsEmpty() || bExplicitPath)
				return Com;

			auto fNormalizeDirectory = [](NStr::CWStr const &_Directory) -> NStr::CWStr
				{
					NStr::CWStr Result;
					DWORD nCapacity = MAX_PATH + 1;
					for (;;)
					{
						DWORD nRequired = GetFullPathNameW(_Directory, nCapacity, Result.f_GetStr(nCapacity), nullptr);
						if (!nRequired)
							return {};
						if (nRequired < nCapacity)
							return Result.f_TrimRight("\\");

						nCapacity = nRequired;
					}
				}
			;

			auto ComDirectory = fNormalizeDirectory(NFile::CFile::fs_GetPath(Com).f_ReplaceChar('/', '\\') + "\\");
			auto ExeDirectory = fNormalizeDirectory(NFile::CFile::fs_GetPath(Exe).f_ReplaceChar('/', '\\') + "\\");
			if (ComDirectory.f_IsEmpty() || ExeDirectory.f_IsEmpty())
				return {};

			for (auto const &Directory : Directories)
			{
				auto NormalizedDirectory = fNormalizeDirectory(Directory);
				if (NormalizedDirectory.f_CmpNoCase(ComDirectory) == 0)
					return Com;
				if (NormalizedDirectory.f_CmpNoCase(ExeDirectory) == 0)
					return Exe;
			}

			return {};
		}

		bool fg_SetLimitsOnJob(NMib::NProcess::CProcessLaunchParams const &_LaunchParams, HANDLE _Job, NStr::CStr &_Errors)
		{
			bool bFailedLaunch = false;
			JOBOBJECT_BASIC_LIMIT_INFORMATION LimitInfo;
			NMemory::fg_MemClear(LimitInfo);

			DWORD ReturnLength;
			QueryInformationJobObject(_Job, JobObjectBasicLimitInformation, &LimitInfo, sizeof(LimitInfo), &ReturnLength);

			bool bSetLimits = false;

			if (_LaunchParams.m_CPUUsage > 0.0f)
			{
				/*
				JOBOBJECT_CPU_RATE_CONTROL_INFORMATION RateInfo;

				// This does not really work...

				fg_MemClear(RateInfo);

				DWORD ReturnLength;
				bool bInfo = QueryInformationJobObject(_Job, JobObjectCpuRateControlInformation, &RateInfo, sizeof(RateInfo), &ReturnLength);

				RateInfo.ControlFlags |= JOB_OBJECT_CPU_RATE_CONTROL_ENABLE | JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP;
				RateInfo.CpuRate = fg_Clamp((_LaunchParams.m_CPUUsage * fp32(100.0f)).f_ToIntRound(), 1, 100*100);
				//RateInfo.CpuRate = 50;

				BOOL bOK = SetInformationJobObject(_Job, JobObjectCpuRateControlInformation, &RateInfo, sizeof(RateInfo));

				if (!bOK)*/
				{
					// HRESULT Error = GetLastError();
					// DMibDTrace("Error: {}" DMibNewLine, NMib::NPlatform::fg_Win32_GetLastErrorStr(Error));

					DWORD_PTR ProcessAffinity, SystemAffinity;

					BOOL bOK = GetProcessAffinityMask(GetCurrentProcess(), &ProcessAffinity, &SystemAffinity);

					if (bOK)
					{
						DWORD_PTR Affinity = 0;

						{ // Use the last X processors.
							umint nProcessors = fg_NumBitsSet(ProcessAffinity);

							umint nAllowedProcessors = (fp32(nProcessors) * (fp32(_LaunchParams.m_CPUUsage))).f_ToInt();
							nAllowedProcessors = fg_Max(umint(1), nAllowedProcessors);

							DWORD_PTR Bit = DWORD_PTR(1) << (DWORD_PTR)((sizeof(DWORD_PTR) * 8) - 1);

							while (Bit && nAllowedProcessors)
							{
								if (Bit & ProcessAffinity)
								{
									Affinity |= Bit;
									--nAllowedProcessors;
								}
								Bit >>= 1;
							}
						}

						LimitInfo.LimitFlags |= JOB_OBJECT_LIMIT_AFFINITY;
						LimitInfo.Affinity = Affinity;
						bSetLimits = true;
					}
					else
					{
						_Errors += NStr::CStr::CFormat("Failed to get process affinity mask: {}" DMibNewLine) << NMib::NPlatform::fg_Win32_GetLastErrorStr(GetLastError());
						bFailedLaunch = true;
					}
				}
			}
			if (_LaunchParams.m_LaunchPriority == EExecutionPriority_Lowest)
			{
				LimitInfo.LimitFlags |= JOB_OBJECT_LIMIT_SCHEDULING_CLASS;
				LimitInfo.SchedulingClass = 0;
				LimitInfo.LimitFlags |= JOB_OBJECT_LIMIT_PRIORITY_CLASS;
				LimitInfo.PriorityClass = IDLE_PRIORITY_CLASS;
				LimitInfo.LimitFlags |= JOB_OBJECT_LIMIT_PRIORITY_CLASS;
				bSetLimits = true;
			}
			if (bSetLimits)
			{
				BOOL bOK = SetInformationJobObject(_Job, JobObjectBasicLimitInformation, &LimitInfo, sizeof(LimitInfo));
				if (!bOK)
				{
					_Errors += NStr::CStr::CFormat("Failed to set job affinity mask: {}" DMibNewLine) << NMib::NPlatform::fg_Win32_GetLastErrorStr(GetLastError());
					bFailedLaunch = true;
				}
			}
			return bFailedLaunch;
		}

		void CConsoleRedirector::f_StopProcess()
		{
			using namespace NStr;

			if (!mp_LastLaunchOptions.m_bCreateNewProcessGroup && mp_LastLaunchOptions.m_RunAsUser.f_IsEmpty())
				DMibError("Cannot stop a process without m_bCreateNewProcessGroup set on Windows");

			if (fg_GetSys()->f_IsDll() || (mp_LastLaunchOptions.m_RunAsUser.f_IsEmpty() && GetConsoleWindow()))
			{
				if (!GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, mp_ProcessID))
					DMibError("Failed to send GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT): {}"_f << NMib::NPlatform::fg_Win32_GetLastErrorStr());
				return;
			}

			// We launch ourselves to be able to attach to the console in the same context as the launched application
			NStr::CStr StdErr;
			NStr::CStr StdOut;
			uint32 ExitCode = 1;

			CProcessLaunchParams LaunchParams;
			LaunchParams.m_bMergeEnvironment = true;
			LaunchParams.m_Environment["MalterlibLaunchStopProcess"] = NStr::CStr::fs_ToStr(mp_ProcessID);
			LaunchParams.m_RunAsUser = mp_LastLaunchOptions.m_RunAsUser;
			LaunchParams.m_RunAsUserPassword = mp_LastLaunchOptions.m_RunAsUserPassword;
			LaunchParams.m_bShowLaunched = false;

			if (!CProcessLaunch::fs_LaunchBlock(NFile::CFile::fs_GetProgramPath(), "", StdErr, StdOut, ExitCode, LaunchParams))
				DMibError("Failed to launch self to signal stop for process: {}{}"_f << StdOut << StdErr);

			if (ExitCode)
				DMibError("Failed to launch self to signal stop for process (Exit status {}): {}{}"_f << ExitCode << StdOut << StdErr);
		}

		bool CConsoleRedirector::fp_LaunchChild(HANDLE _StdOut, HANDLE _StdIn, HANDLE _StdErr, NStr::CStr &_Errors)
		{
			void *pOldvalue = nullptr;

			if (NLocal::g_OptionalFunctions.m_fWow64DisableWow64FsRedirection)
				NLocal::g_OptionalFunctions.m_fWow64DisableWow64FsRedirection(&pOldvalue);

			auto Cleanup = fg_OnScopeExit
				(
					[&]
					{
						if (NLocal::g_OptionalFunctions.m_fWow64RevertWow64FsRedirection)
							NLocal::g_OptionalFunctions.m_fWow64RevertWow64FsRedirection(pOldvalue);
					}
				)
			;

			bool bCreateJob = false; // Should a windows job be created to manage the new process?
			NContainer::TCVector<COnScopeExitShared> CleanupUserProfiles;

			NStr::CStr Program = mp_LastLaunchOptions.m_Target;
			NStr::CStr LocalPaths;
			if (auto *pPath = mp_LastLaunchOptions.m_Environment.f_FindEqual("PATH"))
				LocalPaths = *pPath;

			NStr::CStr Extension = NFile::CFile::fs_GetExtension(Program);
			if (mp_LastLaunchOptions.m_LaunchType == EProcessLaunchType_Executable && (Extension.f_CmpNoCase("com") == 0 || Extension.f_IsEmpty()))
			{
				NStr::CStr Path = NFile::NPlatform::fg_ConvertFromWindowsPath(fg_FindExecutableWindows(Program, mp_LastLaunchOptions.m_bAllowExecutableLocate, LocalPaths));
#if DMibPPtrBits == 64
				NStr::CStr NewProgramPath = NFile::CFile::fs_AppendPath(NFile::CFile::fs_GetPath(Path), NFile::CFile::fs_GetFileNoExt(Path) + "_x64.exe");
#elif DMibPPtrBits == 32
				NStr::CStr NewProgramPath = NFile::CFile::fs_AppendPath(NFile::CFile::fs_GetPath(Path), NFile::CFile::fs_GetFileNoExt(Path) + "_x86.exe");
#else
				NStr::CStr NewProgramPath = Path;
#endif
				if (NFile::CFile::fs_FileExists(NewProgramPath, NFile::EFileAttrib_File))
				{
					Program = NewProgramPath;
					Extension = NFile::CFile::fs_GetExtension(Program);
				}
			}

			if (mp_LastLaunchOptions.m_LaunchType == EProcessLaunchType_Document || mp_LastLaunchOptions.m_LaunchType == EProcessLaunchType_URL)
			{
				if (mp_LastLaunchOptions.m_Elevation != NMib::NProcess::EProcessLaunchElevation_None)
				{
					_Errors += "Launching a document with elevation or de-elevation is not supported" DMibNewLine;
					return false;
				}

				if (mp_LastLaunchOptions.m_Operation == "open folder")
				{
					mp_LastLaunchOptions.m_Operation = "open";
					mp_LastLaunchOptions.m_Parameters = mp_LastLaunchOptions.m_Target;
					mp_LastLaunchOptions.m_Target = "explorer";
				}

				NStr::CStr Program;
				NStr::CStr Params;
				NStr::CWStr ParamsW = NStr::NPlatform::fg_StrToWindows(mp_LastLaunchOptions.m_Parameters);
				bool bTryCreateProcess = false;
				aint MaxLen = _MAX_PATH;
				NStr::CWStr FileW = NFile::NPlatform::fg_ConvertToWindowsPath(mp_LastLaunchOptions.m_Target, false, MaxLen);
				if
				(
					(mp_LastLaunchOptions.m_Operation == "open" || mp_LastLaunchOptions.m_Operation == "")
					&& NFile::CFile::fs_GetExtension(mp_LastLaunchOptions.m_Target).f_CmpNoCase("exe") == 0
				)
				{
					Program = mp_LastLaunchOptions.m_Target;
					Params = mp_LastLaunchOptions.m_Parameters;
					bTryCreateProcess = true;
				}

				bool bIsURL = false;

				try
				{
					aint iFind = mp_LastLaunchOptions.m_Target.f_FindChar(':');
					if (iFind > 1)
					{
						bIsURL = true;
						FileW = NStr::NPlatform::fg_StrToWindows(mp_LastLaunchOptions.m_Target);
						NStr::CStr URLHandler = mp_LastLaunchOptions.m_Target.f_Left(iFind);
						NMib::NPlatform::CWin32_Registry Registry(NMib::NPlatform::CWin32_Registry::ERegRoot_Classes);

						if ((mp_LastLaunchOptions.m_Operation == "open" || mp_LastLaunchOptions.m_Operation == "") && Registry.f_KeyExists(URLHandler))
						{
							if (Registry.f_ValueExists(URLHandler, "URL Protocol"))
							{
								NStr::CStr Command = URLHandler + "\\shell\\open\\command";
								if (Registry.f_KeyExists(Command))
								{
									NStr::CStr LocalPath = Registry.f_Read_Str(Command, "");

									Program = fg_GetWinPathSepEscaped(LocalPath, " ");
									Program = fg_ExpandEnvironmentVars(Program);

									if (NFile::CFile::fs_FileExists(Program) && LocalPath.f_Find("%1") >= 0)
									{
										Params = LocalPath.f_Replace("%1", mp_LastLaunchOptions.m_Target);
										if (Params.f_FindChar('%') < 0)
											bTryCreateProcess = true;
									}
								}
							}
						}
					}
				}
				catch (NException::CException const &)
				{
				}

				if (!bTryCreateProcess && !bIsURL)
				{
					try
					{
						NMib::NPlatform::CWin32_Registry Registry(NMib::NPlatform::CWin32_Registry::ERegRoot_Classes);

						NStr::CStr ExtClass = "." + NFile::CFile::fs_GetExtension(mp_LastLaunchOptions.m_Target);

						if (Registry.f_KeyExists(ExtClass))
						{
							NStr::CStr Class = Registry.f_Read_Str(ExtClass, "");

							NStr::CStr Command = Class + "\\shell\\" + mp_LastLaunchOptions.m_Operation + "\\command";
							NStr::CStr DDECommand = Class + "\\shell\\" + mp_LastLaunchOptions.m_Operation + "\\ddeexec";
							if (Registry.f_KeyExists(DDECommand))
							{
								MaxLen = 218; // Hack for making XLS documents open!!!
								FileW = NFile::NPlatform::fg_ConvertToWindowsPath(mp_LastLaunchOptions.m_Target, false, MaxLen);
							}
							else if (Registry.f_KeyExists(Command))
							{
								NStr::CStr LocalPath = Registry.f_Read_Str(Command, "");

								Program = fg_GetWinPathSepEscaped(LocalPath, " ");
								Program = fg_ExpandEnvironmentVars(Program);

								if (NFile::CFile::fs_FileExists(Program) && LocalPath.f_Find("%1") >= 0)
								{
									Params = LocalPath.f_Replace("%1", NStr::CStr(NFile::NPlatform::fg_ConvertToWindowsPath(mp_LastLaunchOptions.m_Target, false)));
									if (Params.f_FindChar('%') < 0)
										bTryCreateProcess = true;
								}
							}
						}
					}
					catch (NException::CException const &)
					{
					}
				}

				if (bTryCreateProcess && (FileW.f_GetLen() > MaxLen || ParamsW.f_GetLen() >= 2048))
				{
					PROCESS_INFORMATION ProcessInfo;
					STARTUPINFOW StartupInfo;
					NMemory::fg_MemClear(ProcessInfo);
					NMemory::fg_MemClear(StartupInfo);

					StartupInfo.cb = sizeof(STARTUPINFOW);
					StartupInfo.wShowWindow = mp_LastLaunchOptions.m_bShowLaunched ? SW_SHOWNORMAL : SW_HIDE;
					StartupInfo.dwFlags = STARTF_USESHOWWINDOW;

					if (mp_LastLaunchOptions.m_bDisplayBusyCursor)
						StartupInfo.dwFlags |= STARTF_FORCEONFEEDBACK;
					else
						StartupInfo.dwFlags |= STARTF_FORCEOFFFEEDBACK;

					// Launch the child process.
					NStr::CWStr ParamsW = NStr::NPlatform::fg_StrToWindows("\"" + NFile::NPlatform::fg_ConvertToWindowsPath(Program, true) + "\" " + Params);
					NStr::CWStr DirectoryW = NFile::NPlatform::fg_ConvertToWindowsPath(mp_LastLaunchOptions.m_WorkingDirectory, true);
					{
						if
						(
							::CreateProcessW
							(
								NFile::NPlatform::fg_ConvertToWindowsPath(Program, true)
								, ParamsW.f_GetStrUniqueWritable()
								, nullptr
								, nullptr
								, false
								, CREATE_UNICODE_ENVIRONMENT
								, nullptr
								, (!DirectoryW.f_IsEmpty() ? DirectoryW.f_GetStr() : nullptr)
								, &StartupInfo
								, &ProcessInfo
							)
						)
						{
							mp_ChildProcess = ProcessInfo.hProcess;
							mp_ProcessID = ProcessInfo.dwProcessId;
							// Close any unuseful handles
							::CloseHandle(ProcessInfo.hThread);

							return true;
						}
						else
						{
							_Errors += NStr::CStr::CFormat("CreateProcessW failed: {}" DMibNewLine) << NMib::NPlatform::fg_Win32_GetLastErrorStr(GetLastError());
							return false;
						}
					}
				}
				{
					SHELLEXECUTEINFOW ExecInfo;

					NStr::CWStr Operation = NStr::NPlatform::fg_StrToWindows(mp_LastLaunchOptions.m_Operation);
					NStr::CWStr File = FileW;
					NStr::CWStr Params = NStr::NPlatform::fg_StrToWindows(mp_LastLaunchOptions.m_Parameters);
					NStr::CWStr Directory = NFile::NPlatform::fg_ConvertToWindowsPath(mp_LastLaunchOptions.m_WorkingDirectory, false);

					//DDTrace("FileLen: {}\n", File.f_GetLen());

					NMemory::fg_MemClear(ExecInfo);
					ExecInfo.cbSize = sizeof(ExecInfo);
					ExecInfo.lpVerb = !Operation.f_IsEmpty() ? Operation.f_GetStr() : nullptr;
					ExecInfo.lpFile = !File.f_IsEmpty() ? File.f_GetStr() : nullptr;
					ExecInfo.hwnd = nullptr;
					ExecInfo.nShow = mp_LastLaunchOptions.m_bShowLaunched ? SW_SHOWNORMAL : SW_HIDE;
					ExecInfo.lpParameters = !Params.f_IsEmpty() ? Params.f_GetStr() : nullptr;
					ExecInfo.lpDirectory = !Directory.f_IsEmpty() ? Directory.f_GetStr() : nullptr;
					ExecInfo.fMask = SEE_MASK_UNICODE | SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC; // SEE_MASK_ASYNCOK
					ExecInfo.fMask |= SEE_MASK_NOCLOSEPROCESS;

					if (!ShellExecuteExW(&ExecInfo))
					{
						_Errors += NStr::CStr::CFormat("ShellExecuteExW failed: {}" DMibNewLine) << NMib::NPlatform::fg_Win32_GetLastErrorStr(GetLastError());
						return false;
					}
					else
					{
						if (ExecInfo.hProcess)
						{
							mp_ChildProcess = ExecInfo.hProcess;
							mp_ProcessID = GetProcessId(ExecInfo.hProcess);
						}
						else
							mp_ProcessID = 0;
						return true;
					}
				}
			}
			else if (mp_LastLaunchOptions.m_Elevation == NMib::NProcess::EProcessLaunchElevation_Elevate)
			{
				auto CurrentElevation = NMib::NProcess::CProcessLaunch::fs_GetElevation();
				if (CurrentElevation == NMib::NProcess::EProcessElevation_None)
				{
					_Errors += "Launching process elevated is not supported." DMibNewLine;
					return false;
				}
				else if (CurrentElevation == NMib::NProcess::EProcessElevation_IsElevated)
				{
					_Errors += "Process is already elevated and therefore cannot be launched elevated." DMibNewLine;
					return false;
				}

				// When stdio redirection is enabled, create named pipes and pass
				// pipe base name to child via --OutputPID. The child connects its
				// stdio to these pipes early in startup.
				HANDLE StdOutPipe = nullptr;
				HANDLE StdErrPipe = nullptr;
				HANDLE StdInPipe = nullptr;
				NStr::CStr PipeBaseName;

				if (mp_LastLaunchOptions.m_bEnableStdRedirection && mp_LastLaunchOptions.m_bStdOutPID)
				{
					// Generate cryptographically random pipe base name
					PipeBaseName = NStr::CStr::CFormat("\\\\.\\pipe\\MalterlibStdLaunch_{}") << NCryptography::fg_RandomID();

					// Create stdout pipe (parent reads, child writes)
					NStr::CWStr StdOutPipeName = NStr::NPlatform::fg_StrToWindows(PipeBaseName + "_StdOut");
					StdOutPipe = CreateNamedPipeW
						(
							StdOutPipeName.f_GetStr()
							, PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE
							, PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS
							, 1
							, 4096
							, 4096
							, 120 * 1000
							, nullptr
						)
					;

					if (!StdOutPipe || StdOutPipe == INVALID_HANDLE_VALUE)
					{
						StdOutPipe = nullptr;
						_Errors += NStr::CStr::CFormat("CreateNamedPipe (StdOut) failed: {}" DMibNewLine) << NMib::NPlatform::fg_Win32_GetLastErrorStr(GetLastError());
						return false;
					}

					// Create stderr pipe if separate stderr requested
					if (mp_LastLaunchOptions.m_bSeparateStdErr)
					{
						NStr::CWStr StdErrPipeName = NStr::NPlatform::fg_StrToWindows(PipeBaseName + "_StdErr");
						StdErrPipe = CreateNamedPipeW
							(
								StdErrPipeName.f_GetStr()
								, PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE
								, PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS
								, 1
								, 4096
								, 4096
								, 120 * 1000
								, nullptr
							)
						;

						if (!StdErrPipe || StdErrPipe == INVALID_HANDLE_VALUE)
						{
							StdErrPipe = nullptr;
							CloseHandle(StdOutPipe);
							_Errors += NStr::CStr::CFormat("CreateNamedPipe (StdErr) failed: {}" DMibNewLine) << NMib::NPlatform::fg_Win32_GetLastErrorStr(GetLastError());
							return false;
						}
					}

					// Create stdin pipe (parent writes, child reads)
					// FILE_FLAG_OVERLAPPED needed for ConnectNamedPipe with WaitForMultipleObjects
					NStr::CWStr StdInPipeName = NStr::NPlatform::fg_StrToWindows(PipeBaseName + "_StdIn");
					StdInPipe = CreateNamedPipeW
						(
							StdInPipeName.f_GetStr()
							, PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE
							, PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS
							, 1
							, 4096
							, 4096
							, 120 * 1000
							, nullptr
						)
					;

					if (!StdInPipe || StdInPipe == INVALID_HANDLE_VALUE)
					{
						StdInPipe = nullptr;
						CloseHandle(StdOutPipe);
						if (StdErrPipe)
							CloseHandle(StdErrPipe);
						_Errors += NStr::CStr::CFormat("CreateNamedPipe (StdIn) failed: {}" DMibNewLine) << NMib::NPlatform::fg_Win32_GetLastErrorStr(GetLastError());
						return false;
					}
				}

				auto PipeCleanup = fg_OnScopeExit
					(
						[&]
						{
							if (StdOutPipe)
								CloseHandle(StdOutPipe);
							if (StdErrPipe)
								CloseHandle(StdErrPipe);
							if (StdInPipe)
								CloseHandle(StdInPipe);
						}
					)
				;

				SHELLEXECUTEINFOW ExecInfo;

				NStr::CWStr File = fg_FindExecutableWindows(Program, mp_LastLaunchOptions.m_bAllowExecutableLocate, LocalPaths);
				NStr::CWStr Params = NStr::NPlatform::fg_StrToWindows(mp_LastLaunchOptions.m_Parameters);
				if (!PipeBaseName.f_IsEmpty())
				{
					if (!Params.f_IsEmpty())
						Params += " ";
					Params += "--OutputPID ";
					Params += PipeBaseName;
				}
				NStr::CWStr Directory
					= !mp_LastLaunchOptions.m_WorkingDirectory.f_IsEmpty() ? NFile::NPlatform::fg_ConvertToWindowsPath(mp_LastLaunchOptions.m_WorkingDirectory, true) : NStr::CWStr()
				;

				//DDTrace("FileLen: {}\n", File.f_GetLen());

				NMemory::fg_MemClear(ExecInfo);
				ExecInfo.cbSize = sizeof(ExecInfo);
				ExecInfo.lpVerb = str_utf16("runas");
				ExecInfo.lpFile = !File.f_IsEmpty() ? File.f_GetStr() : nullptr;
				ExecInfo.hwnd = nullptr;
				ExecInfo.nShow = mp_LastLaunchOptions.m_bShowLaunched ? SW_SHOWNORMAL : SW_HIDE;
				ExecInfo.lpParameters = !Params.f_IsEmpty() ? Params.f_GetStr() : nullptr;
				if (!Directory.f_IsEmpty())
					ExecInfo.lpDirectory = Directory;
				ExecInfo.fMask = SEE_MASK_UNICODE | SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC; // SEE_MASK_ASYNCOK
				ExecInfo.fMask |= SEE_MASK_NOCLOSEPROCESS;

				if (!ShellExecuteExW(&ExecInfo))
				{
					_Errors += NStr::CStr::CFormat("ShellExecuteExW failed: {}" DMibNewLine) << NMib::NPlatform::fg_Win32_GetLastErrorStr(GetLastError());
					return false;
				}

				mp_ChildProcess = ExecInfo.hProcess;
				mp_ProcessID = GetProcessId(ExecInfo.hProcess);

				// Connect named pipes and verify client PID.
				// Uses overlapped ConnectNamedPipe + WaitForMultipleObjects so we
				// detect child death instead of blocking forever.
				if (!PipeBaseName.f_IsEmpty())
				{
					auto fConnectAndVerify = [&](HANDLE _Pipe, char const *_pName) -> bool
						{
							while (true)
							{
								OVERLAPPED Overlapped = {};
								Overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
								if (!Overlapped.hEvent)
								{
									_Errors += NStr::CStr::CFormat("CreateEvent ({}) failed: {}" DMibNewLine) << _pName << NMib::NPlatform::fg_Win32_GetLastErrorStr(GetLastError());
									return false;
								}
								auto EventCleanup = fg_OnScopeExit
									(
										[&]
										{
											CloseHandle(Overlapped.hEvent);
										}
									)
								;

								BOOL bConnected = ConnectNamedPipe(_Pipe, &Overlapped);
								DWORD Error = GetLastError();

								if (!bConnected && Error == ERROR_IO_PENDING)
								{
									// Wait for either pipe connection or child process exit
									HANDLE WaitHandles[2] = {Overlapped.hEvent, mp_ChildProcess};
									DWORD WaitResult = WaitForMultipleObjects(2, WaitHandles, FALSE, 120 * 1000);

									if (WaitResult == WAIT_OBJECT_0 + 1)
									{
										CancelIo(_Pipe);
										_Errors += NStr::CStr::CFormat("Child process exited before connecting to {} pipe" DMibNewLine) << _pName;
										return false;
									}
									else if (WaitResult == WAIT_TIMEOUT)
									{
										CancelIo(_Pipe);
										_Errors += NStr::CStr::CFormat("Timed out waiting for child to connect to {} pipe" DMibNewLine) << _pName;
										return false;
									}
									else if (WaitResult != WAIT_OBJECT_0)
									{
										CancelIo(_Pipe);
										_Errors
											+= NStr::CStr::CFormat("WaitForMultipleObjects ({}) failed: {}" DMibNewLine)
											<< _pName
											<< NMib::NPlatform::fg_Win32_GetLastErrorStr(GetLastError())
										;
										return false;
									}
								}
								else if (!bConnected && Error != ERROR_PIPE_CONNECTED)
								{
									_Errors += NStr::CStr::CFormat("ConnectNamedPipe ({}) failed: {}" DMibNewLine) << _pName << NMib::NPlatform::fg_Win32_GetLastErrorStr(Error);
									return false;
								}

								// Verify the connecting process is the child we launched
								ULONG ClientPid = 0;
								if (GetNamedPipeClientProcessId(_Pipe, &ClientPid) && ClientPid == mp_ProcessID)
									return true;

								// Wrong process connected, disconnect and retry
								DisconnectNamedPipe(_Pipe);
							}
						}
					;

					if (!fConnectAndVerify(StdOutPipe, "StdOut"))
						return false;

					if (StdErrPipe)
					{
						if (!fConnectAndVerify(StdErrPipe, "StdErr"))
							return false;
					}

					if (!fConnectAndVerify(StdInPipe, "StdIn"))
						return false;

					// Transfer pipe ownership to member variables
					mp_StdoutRead = StdOutPipe;
					StdOutPipe = nullptr;

					if (StdErrPipe)
					{
						mp_StderrRead = StdErrPipe;
						StdErrPipe = nullptr;
					}

					{
						DMibLock(mp_PipeLock);
						mp_StdinWrite = StdInPipe;
						StdInPipe = nullptr;
					}
				}

				PipeCleanup.f_Clear();
				return true;
			}
			else
			{
				HANDLE Token = nullptr;

				COnScopeExitShared pCleanupToken;

				DWORD CreateProcessFlags = 0;

				if (mp_LastLaunchOptions.m_bCreateNewProcessGroup)
				{
					CreateProcessFlags |= CREATE_NEW_PROCESS_GROUP;
					if (!mp_LastLaunchOptions.m_bSandboxed)
						CreateProcessFlags |= CREATE_SUSPENDED; // So that its job holds every process it starts
				}

				auto Environment = mp_LastLaunchOptions.m_Environment;

				NStr::CWStr ProgramPathFull = fg_FindExecutableWindows(Program, mp_LastLaunchOptions.m_bAllowExecutableLocate, LocalPaths);

				if (ProgramPathFull.f_IsEmpty())
				{
					using namespace NStr;
					_Errors += "Failed to find an executable for '{}'\n"_f << Program;
					return false;
				}

				NStr::CStr SandboxDll;
				NStr::CStr SandboxFullPath;
				DWORD BinaryType;
				bool bRunAsUser = false;
				CSystemEnvironment UserEnvironment;

				if (mp_LastLaunchOptions.m_Elevation == NMib::NProcess::EProcessLaunchElevation_DeElevate)
				{
					auto CurrentElevation = NMib::NProcess::CProcessLaunch::fs_GetElevation();
					if (CurrentElevation == NMib::NProcess::EProcessElevation_None)
					{
						_Errors += "Launching process de-elevated is not supported." DMibNewLine;
						return false;
					}
					else if (CurrentElevation == NMib::NProcess::EProcessElevation_IsNotElevated)
					{
						_Errors += "Process is not elevated and therefore cannot be launched de-elevated." DMibNewLine;
						return false;
					}
					uint32 ActiveSessionID = fg_GetActiveSessionID();
					CProcessEntry RootProcess;
					HANDLE ProcessSnapshot;
					PROCESSENTRY32 ProcessEntry;

					HANDLE SessionToken = nullptr;
					if (WTSQueryUserToken(ActiveSessionID, &SessionToken) == 0)
					{
						uint32 Error = GetLastError();
						_Errors += NStr::CStr::CFormat("{}" DMibNewLine) << NMib::NPlatform::fg_Win32_GetLastErrorStr(Error);
					}

					if (SessionToken != nullptr)
					{
						if (DuplicateTokenEx(SessionToken, MAXIMUM_ALLOWED, NULL, SecurityImpersonation, TokenPrimary, &Token) == 0)
						{
							uint32 Error = GetLastError();
							_Errors += NStr::CStr::CFormat("{}" DMibNewLine) << NMib::NPlatform::fg_Win32_GetLastErrorStr(Error);
						}

						CloseHandle(SessionToken);
					}

					if (Token == nullptr)
					{
						for (int iPass = 0; iPass < 3 && Token == nullptr; ++iPass)
						{
							// Take a snapshot of all processes in the system.
							ProcessSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
							if (ProcessSnapshot != INVALID_HANDLE_VALUE)
							{
								ProcessEntry.dwSize = sizeof(PROCESSENTRY32);

								if (Process32First(ProcessSnapshot, &ProcessEntry))
								{
									HANDLE pThisProcess = nullptr;
									do
									{
										uint32 SessionID = 0xFFFFffff;
										ProcessIdToSessionId(ProcessEntry.th32ProcessID, &SessionID);

										if (SessionID != ActiveSessionID)
											continue;

										pThisProcess = OpenProcess(MAXIMUM_ALLOWED, false, ProcessEntry.th32ProcessID);
										if (!pThisProcess)
											continue;

										auto Elevation = fg_Process_GetElevation(pThisProcess);
										if (Elevation != NMib::NProcess::EProcessElevation_IsNotElevated)
											continue;
										NStr::CStr Name = fg_GetProcessName(pThisProcess);
										if ((iPass == 0 && Name.f_CmpNoCase("explorer.exe") == 0) || (iPass == 1 && Name.f_CmpNoCase("LogonUI.exe") == 0) || (iPass == 2 && Name != ""))
										{
											HANDLE ProcessToken;
											if (OpenProcessToken(pThisProcess, TOKEN_DUPLICATE, &ProcessToken))
											{
												Token = nullptr;

												if (!DuplicateTokenEx(ProcessToken, MAXIMUM_ALLOWED, NULL, SecurityImpersonation, TokenPrimary, &Token))
												{
													// DMibDTrace("DuplicateTokenEx failed with {}" DMibNewLine, NMib::NPlatform::fg_Win32_GetLastErrorStr(GetLastError()));
												}
												CloseHandle(ProcessToken);
											}
											if (Token)
												break;
										}
										CloseHandle(pThisProcess);
									}
									while (Process32Next(ProcessSnapshot, &ProcessEntry));
									if (pThisProcess != nullptr)
										CloseHandle(pThisProcess);
								}
								CloseHandle(ProcessSnapshot);
							}
						}
					}

					if (!Token)
					{
						_Errors += "When de-elevating, found no process to copy security token from." DMibNewLine;
						return false;
					}

					pCleanupToken = g_OnScopeExitShared / [Token]
						{
							if (Token)
								CloseHandle(Token);
						}
					;
				}
				else if (!mp_LastLaunchOptions.m_RunAsUser.f_IsEmpty())
				{
					auto &SubSystem = *g_SubSystem_Process_Platform_Windows_Launch;
					DMibLock(SubSystem.m_UserLaunchLock);

					using namespace NStr;
					CWStr UserName = mp_LastLaunchOptions.m_RunAsUser;
					CWStrSecure Password = mp_LastLaunchOptions.m_RunAsUserPassword;
					//HANDLE hLogonToken;
					PSID pLogonSid = nullptr;
					if (!LogonUserExW(UserName.f_GetStr(), L".", Password.f_GetStr(), LOGON32_LOGON_INTERACTIVE, LOGON32_PROVIDER_DEFAULT, &Token, &pLogonSid, nullptr, nullptr, nullptr))
					{
						_Errors += "When launching as user, failed to login as user '{}': {}{\n}"_f << UserName << NMib::NPlatform::fg_Win32_GetLastErrorStr();
						return false;
					}

					pCleanupToken = g_OnScopeExitShared / [Token]
						{
							if (Token)
								CloseHandle(Token);
						}
					;

					auto pCleanupLogonSid = g_OnScopeExitShared / [pLogonSid]
						{
							if (pLogonSid)
								LocalFree(pLogonSid);
						}
					;

					PROFILEINFOW ProfileInfo;
					NMemory::fg_MemClear(ProfileInfo);
					ProfileInfo.dwSize = sizeof(ProfileInfo);
					ProfileInfo.dwFlags = PI_NOUI;
					ProfileInfo.lpUserName = UserName.f_GetStrWritable();
					if (!LoadUserProfileW(Token, &ProfileInfo))
					{
						_Errors += "When launching as user, failed to load user profile: {}{\n}"_f << NMib::NPlatform::fg_Win32_GetLastErrorStr();
						return false;
					}

					CleanupUserProfiles.f_Insert
						(
							g_OnScopeExitShared / [Token, Profile = ProfileInfo.hProfile, pCleanupToken]
							{
								if (!UnloadUserProfile(Token, Profile))
									DMibDTrace("When launching as user, failed to unload user profile: {}{\n}", NMib::NPlatform::fg_Win32_GetLastErrorStr());
							}
						)
					;

					//DuplicateTokenEx(hLogonToken, );

					void *pEnvironmentVoid;
					if (!CreateEnvironmentBlock(&pEnvironmentVoid, Token, false))
					{
						_Errors += "When launching as user, failed to create environment block: {}{\n}"_f << NMib::NPlatform::fg_Win32_GetLastErrorStr();
						return false;
					}

					auto CleanupEnvBlock = g_OnScopeExit / [&]
						{
							DestroyEnvironmentBlock(pEnvironmentVoid);
						}
					;

					for (ch16 *pEnvironment = (ch16 *)pEnvironmentVoid; *pEnvironment;)
					{
						CWStr Line{pEnvironment};
						umint LineLength = Line.f_GetLen();
						auto Key = fg_GetStrSep(Line, "=");
						UserEnvironment[CStr(Key)] = Line;
						pEnvironment += LineLength + 1;
					}

					HWINSTA CurrentWindowStation = GetProcessWindowStation();
					if (!CurrentWindowStation)
					{
						_Errors += "When launching as user, failed to get current window station: {}{\n}"_f << NMib::NPlatform::fg_Win32_GetLastErrorStr();
						return false;
					}

					HWINSTA WindowStation = OpenWindowStationW(L"winsta0", false, READ_CONTROL | WRITE_DAC);

					if (!WindowStation)
					{
						_Errors += "When launching as user, failed to open window station: {}{\n}"_f << NMib::NPlatform::fg_Win32_GetLastErrorStr();
						return false;
					}

					auto pCleanupWindowStation = g_OnScopeExitShared / [WindowStation]
						{
							CloseWindowStation(WindowStation);
						}
					;

					HDESK Desktop;
					{
						if (!SetProcessWindowStation(WindowStation))
						{
							_Errors += "When launching as user, failed to temporarily set window station: {}{\n}"_f << NMib::NPlatform::fg_Win32_GetLastErrorStr();
							return false;
						}

						auto CleanupWindowStation = g_OnScopeExit / [&]
							{
								if (!SetProcessWindowStation(CurrentWindowStation))
									DMibDTrace("When launching as user, failed to restore window station: {}{\n}", NMib::NPlatform::fg_Win32_GetLastErrorStr());
							}
						;

						Desktop = OpenDesktopW(L"default", 0, FALSE, READ_CONTROL | WRITE_DAC | DESKTOP_WRITEOBJECTS | DESKTOP_READOBJECTS);
					}

					if (!Desktop)
					{
						_Errors += "When launching as user, failed to open desktop: {}{\n}"_f << NMib::NPlatform::fg_Win32_GetLastErrorStr();
						return false;
					}

					auto pCleanupDesktop = g_OnScopeExitShared / [Desktop]
						{
							CloseDesktop(Desktop);
						}
					;

					bool bAddedToWindowStation = false;
					if (!fg_ChangeAceToWindowStation(WindowStation, pLogonSid, false, bAddedToWindowStation))
					{
						_Errors += "When launching as user, failed to add access to window station: {}{\n}"_f << NMib::NPlatform::fg_Win32_GetLastErrorStr();
						return false;
					}

					if (bAddedToWindowStation)
					{
						CleanupUserProfiles.f_Insert
							(
								g_OnScopeExitShared / [pCleanupWindowStation, pCleanupLogonSid, WindowStation, pLogonSid]
								{
									bool bAdded;
									if (!fg_ChangeAceToWindowStation(WindowStation, pLogonSid, true, bAdded))
										DMibDTrace("When launching as user, failed to remove access from window station: {}{\n}", NMib::NPlatform::fg_Win32_GetLastErrorStr());
								}
							)
						;
					}

					bool bAddedToDesktop = false;

					if (!fg_ChangeAceToDesktop(Desktop, pLogonSid, false, bAddedToDesktop))
					{
						_Errors += "When launching as user, failed to add access to desktop: {}{\n}"_f << NMib::NPlatform::fg_Win32_GetLastErrorStr();
						return false;
					}

					if (bAddedToDesktop)
					{
						CleanupUserProfiles.f_Insert
							(
								g_OnScopeExitShared / [pCleanupDesktop, pCleanupLogonSid, Desktop, pLogonSid]
								{
									bool bAdded;
									if (!fg_ChangeAceToDesktop(Desktop, pLogonSid, true, bAdded))
										DMibDTrace("When launching as user, failed to remove access from desktop: {}{\n}", NMib::NPlatform::fg_Win32_GetLastErrorStr());
								}
							)
						;
					}

					//GENERIC_READ
					//WINSTA_ALL_ACCESS
					bRunAsUser = true;
				}

				if (mp_LastLaunchOptions.m_bSandboxed)
				{
					if (!mp_LastLaunchOptions.m_SandboxRoots.f_IsEmpty())
					{
						if (GetBinaryTypeW(ProgramPathFull, &BinaryType))
						{
							if (BinaryType == SCS_64BIT_BINARY)
								SandboxDll = "MalterlibSandbox_x64.dll";
							else if (BinaryType == SCS_32BIT_BINARY)
								SandboxDll = "MalterlibSandbox_x86.dll";

							NStr::CStr Path = fg_GetSandboxDir(_Errors);

							if (Path.f_IsEmpty())
								return false;

							SandboxFullPath = Path + "/" + SandboxDll;

							if (!NFile::CFile::fs_FileExists(SandboxFullPath))
							{
								_Errors += "Sandbox dll not found" DMibNewLine;
								return false;
							}
						}
						else
						{
							_Errors += NStr::CStr::CFormat("Binary type of executable could not be determined () {\n}") << ProgramPathFull;
							return false;
						}

						NMib::NStr::CStr Remappings;
						for (auto iRoot = mp_LastLaunchOptions.m_SandboxRoots.f_GetIterator(); iRoot; ++iRoot)
						{
							fg_AddStrSep(Remappings, NStr::CStr(NStr::CStr::CFormat("{}={}") << iRoot.f_GetKey() << *iRoot), ";");
							if (mp_LastLaunchOptions.m_bCopyRootToSandbox)
							{
								try
								{
									auto Drive = NMib::NFile::CFile::fs_GetDrive(iRoot.f_GetKey());
									auto Files = NMib::NFile::CFile::fs_FindFilesEx(NStr::CStr(Drive + "/*"), NMib::NFile::EFileAttrib_Directory);
									for (auto iFile = Files.f_GetIterator(); iFile; ++iFile)
									{
										NStr::CStr DestFile = NFile::CFile::fs_AppendPath(*iRoot, iFile->m_Path.f_Extract(Drive.f_GetLen() + 1));

										if (!NMib::NFile::CFile::fs_FileExists(DestFile))
											NMib::NFile::CFile::fs_CreateSymbolicLink(iFile->m_Path, DestFile, iFile->m_Attribs, NFile::ESymbolicLinkFlag_ConvertToDevicePath);
									}
								}
								catch (NException::CException const &_Exception)
								{
									_Errors += NStr::CStr::CFormat("Failed to copy sandbox root files/directories: {}" DMibNewLine) << _Exception.f_GetErrorStr();
									return false;
								}
							}
						}
						Environment["MalterlibSandboxRemappings"] = Remappings;
					}
					CreateProcessFlags |= CREATE_SUSPENDED;
					bCreateJob = true; // Sandbox needs job to kill all processes.
				}

				if (mp_LastLaunchOptions.m_CPUUsage > 0.0f)
				{
					CreateProcessFlags |= CREATE_SUSPENDED;
					bCreateJob = true; // Job used to cpu limit process.
				}

				bool bIsWin8 = false;
				{
					int Major, Minor, Fix;
					EOperatingSystemArch Arch;
					if (NMib::NSys::fg_System_GetOperatingSystemVersion(Major, Minor, Fix, Arch))
					{
						if ((Major == 6 && Minor >= 2) || (Major > 6))
							bIsWin8 = true;
					}
				}

				if (bCreateJob)
				{
					if (!bIsWin8)
						CreateProcessFlags |= CREATE_BREAKAWAY_FROM_JOB;
				}

				mp_ProcessID = 0;
				PROCESS_INFORMATION ProcessInfo;
				STARTUPINFOW StartupInfo;

				DMibSafeCheck(mp_ChildProcess == nullptr, "Error");

				NContainer::TCVector<ch16> NewEnvStrs;

				CSystemEnvironment NewEnvironment;
				if (mp_LastLaunchOptions.m_bMergeEnvironment)
				{
					CSystemEnvironment OriginalEnvironment;
					if (mp_LastLaunchOptions.m_RunAsUser.f_IsEmpty())
						OriginalEnvironment = fg_GetSys()->f_Environment();
					else
						OriginalEnvironment = UserEnvironment;

					NewEnvironment = Environment;
					NewEnvironment += OriginalEnvironment;
				}
				else if (!Environment.f_IsEmpty())
					NewEnvironment = Environment;
				else
				{
					if (mp_LastLaunchOptions.m_RunAsUser.f_IsEmpty())
						NewEnvironment = fg_GetSys()->f_Environment();
					else
						NewEnvironment = UserEnvironment;
				}

				auto Iter = NewEnvironment.f_GetIterator();

				while (Iter)
				{
					NStr::CWStr WholeStr = NStr::NPlatform::fg_StrToWindows(Iter.f_GetKey() + "=" + *Iter);
					NewEnvStrs.f_Insert(WholeStr.f_GetStr(), WholeStr.f_GetLen() + 1);

					++Iter;
				}
				NewEnvStrs.f_Insert(ch16(0));

				// Set up the start up info struct.
				NMemory::fg_MemClear(StartupInfo);
				StartupInfo.cb = sizeof(STARTUPINFOW);
				StartupInfo.hStdOutput = _StdOut;
				StartupInfo.hStdInput = _StdIn;
				StartupInfo.hStdError = _StdErr;
				StartupInfo.wShowWindow = mp_LastLaunchOptions.m_bShowLaunched ? SW_SHOWNORMAL : SW_HIDE;
				StartupInfo.dwFlags = STARTF_USESHOWWINDOW;
				if (bRunAsUser)
				{
					StartupInfo.lpDesktop = fg_AutoConstCast(L"winsta0\\default");
					CreateProcessFlags |= CREATE_NEW_CONSOLE;
				}

				if (mp_LastLaunchOptions.m_bDisplayBusyCursor)
					StartupInfo.dwFlags |= STARTF_FORCEONFEEDBACK;
				else
					StartupInfo.dwFlags |= STARTF_FORCEOFFFEEDBACK;

				if (mp_LastLaunchOptions.m_bEnableStdRedirection)
					StartupInfo.dwFlags |= STARTF_USESTDHANDLES;

				// Note that dwFlags must include STARTF_USESHOWWINDOW if we
				// use the wShowWindow flags. This also assumes that the
				// CreateProcess() call will use CREATE_NEW_CONSOLE.

				// Launch the child process.

				NStr::CWStr Params = NStr::NPlatform::fg_StrToWindows(ProgramPathFull.f_EscapeStr().f_Replace("\\\\", "\\") + " " + mp_LastLaunchOptions.m_Parameters);
				if (Token)
				{
					if (NLocal::g_OptionalFunctions.m_fCreateProcessWithTokenW)
					{
						if (!bRunAsUser)
						{
							HANDLE ThreadToken = NULL;
							ImpersonateSelf(SecurityImpersonation);
							if (!OpenThreadToken(GetCurrentThread(), MAXIMUM_ALLOWED, false, &ThreadToken))
							{
								NStr::CStr Error = NMib::NPlatform::fg_Win32_GetLastErrorStr(GetLastError());
								_Errors += NStr::CStr::CFormat("OpenThreadToken failed with : {}" DMibNewLine) << Error;
								RevertToSelf();
								return false;
							}
							else
							{
								TOKEN_PRIVILEGES TokenPrivileges;
								TokenPrivileges.PrivilegeCount = 1;
								LookupPrivilegeValueW(NULL, SE_INCREASE_QUOTA_NAME, &TokenPrivileges.Privileges[0].Luid);
								TokenPrivileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
								AdjustTokenPrivileges(ThreadToken, FALSE, &TokenPrivileges, 0, NULL, NULL);
								HRESULT LastError = GetLastError();
								CloseHandle(ThreadToken);
								if (LastError != ERROR_SUCCESS)
								{
									NStr::CStr Error = NMib::NPlatform::fg_Win32_GetLastErrorStr(GetLastError());
									_Errors += NStr::CStr::CFormat("AdjustTokenPrivileges failed with : {}" DMibNewLine) << Error;
									return false;
								}
							}
						}

						ProgramPathFull = NFile::NPlatform::fg_ConvertToWindowsPath(ProgramPathFull, false, 250, true);
						NStr::CWStr WorkingDir = NFile::NPlatform::fg_ConvertToWindowsPath(mp_LastLaunchOptions.m_WorkingDirectory, true, 250, true);

						if
						(
							!NLocal::g_OptionalFunctions.m_fCreateProcessWithTokenW
							(
								Token
								, 0
								, ProgramPathFull
								, Params.f_GetStrUniqueWritable()
								, CREATE_UNICODE_ENVIRONMENT | fg_Win32_TranslateProcessPriority(mp_LastLaunchOptions.m_LaunchPriority) | CreateProcessFlags
								, !NewEnvStrs.f_IsEmpty() ? NewEnvStrs.f_GetArray() : nullptr
								, !WorkingDir.f_IsEmpty() ? WorkingDir.f_GetStr() : nullptr
								, &StartupInfo
								, &ProcessInfo
							)
						)
						{
							if (!bRunAsUser)
								RevertToSelf();
							NStr::CStr Error = NMib::NPlatform::fg_Win32_GetLastErrorStr(GetLastError());
							_Errors += NStr::CStr::CFormat("CreateProcessWithTokenW({}, {}, {}) failed with : {}" DMibNewLine) << ProgramPathFull << Params << WorkingDir << Error;
							return false;
						}
						if (!bRunAsUser)
							RevertToSelf();
					}
					else
					{
						if
						(
							!::CreateProcessAsUserW
							(
								Token
								, ProgramPathFull
								, Params.f_GetStrUniqueWritable()
								, nullptr
								, nullptr
								, true
								, CREATE_UNICODE_ENVIRONMENT | fg_Win32_TranslateProcessPriority(mp_LastLaunchOptions.m_LaunchPriority) | CreateProcessFlags
								, !NewEnvStrs.f_IsEmpty() ? NewEnvStrs.f_GetArray() : nullptr
								, !mp_LastLaunchOptions.m_WorkingDirectory.f_IsEmpty()
								? NFile::NPlatform::fg_ConvertToWindowsPath(mp_LastLaunchOptions.m_WorkingDirectory, true).f_GetStr()
								: nullptr
								, &StartupInfo
								, &ProcessInfo
							)
						)
						{
							NStr::CStr Error = NMib::NPlatform::fg_Win32_GetLastErrorStr(GetLastError());
							_Errors += NStr::CStr::CFormat("CreateProcessW({}, {}) failed with : {}" DMibNewLine) << ProgramPathFull << Params << Error;
							return false;
						}
					}
				}
				else if (!mp_LastLaunchOptions.m_RunAsUser.f_IsEmpty())
				{
					using namespace NStr;
					CWStr UserName = mp_LastLaunchOptions.m_RunAsUser;
					CWStrSecure Password = mp_LastLaunchOptions.m_RunAsUserPassword;
					if
					(
						!::CreateProcessWithLogonW
						(
							UserName.f_GetStr()
							, nullptr
							, Password.f_GetStr()
							, LOGON_WITH_PROFILE
							, ProgramPathFull
							, Params.f_GetStrUniqueWritable()
							, CREATE_UNICODE_ENVIRONMENT | fg_Win32_TranslateProcessPriority(mp_LastLaunchOptions.m_LaunchPriority) | CreateProcessFlags
							, nullptr //, !NewEnvStrs.f_IsEmpty() ? NewEnvStrs.f_GetArray() : nullptr
							, !mp_LastLaunchOptions.m_WorkingDirectory.f_IsEmpty()
							? NFile::NPlatform::fg_ConvertToWindowsPath(mp_LastLaunchOptions.m_WorkingDirectory, true).f_GetStr()
							: nullptr
							, &StartupInfo
							, &ProcessInfo
						)
					)
					{
						NStr::CStr Error = NMib::NPlatform::fg_Win32_GetLastErrorStr(GetLastError());
						_Errors += NStr::CStr::CFormat("CreateProcessWithLogonW(\"{}\", {}) failed with : {}" DMibNewLine) << ProgramPathFull << Params << Error;
						return false;
					}
				}
				else
				{
					if
					(
						!::CreateProcessW
						(
							ProgramPathFull
							, Params.f_GetStrUniqueWritable()
							, nullptr
							, nullptr
							, TRUE
							, CREATE_UNICODE_ENVIRONMENT | fg_Win32_TranslateProcessPriority(mp_LastLaunchOptions.m_LaunchPriority) | CreateProcessFlags
							, !NewEnvStrs.f_IsEmpty() ? NewEnvStrs.f_GetArray() : nullptr
							, !mp_LastLaunchOptions.m_WorkingDirectory.f_IsEmpty()
							? NFile::NPlatform::fg_ConvertToWindowsPath(mp_LastLaunchOptions.m_WorkingDirectory, true).f_GetStr()
							: nullptr
							, &StartupInfo
							, &ProcessInfo
						)
					)
					{
						NStr::CStr Error = NMib::NPlatform::fg_Win32_GetLastErrorStr(GetLastError());
						_Errors += NStr::CStr::CFormat("CreateProcessW(\"{}\", {}) failed with : {}" DMibNewLine) << ProgramPathFull << Params << Error;
						return false;
					}
				}

				bool bFailedLaunch = false;

				if (bCreateJob)
				{
					if (!bFailedLaunch)
					{
						bool bSetOnParent = false;
						HANDLE ParentJob = nullptr;
						auto JobCleanupParent = fg_OnScopeExit
							(
								[&]()
								{
									if (ParentJob)
										CloseHandle(ParentJob);
								}
							)
						;
						if (bIsWin8 && !mp_LastLaunchOptions.m_ProcessGroup.f_IsEmpty())
						{
							NStr::CWStr JobName = mp_LastLaunchOptions.m_ProcessGroup;
							ParentJob = CreateJobObjectW(nullptr, JobName);
							if (ParentJob)
							{
								bool bFailed = false;
								if (GetLastError() != ERROR_ALREADY_EXISTS)
								{
									NStr::CStr Errors;
									if (fg_SetLimitsOnJob(mp_LastLaunchOptions, ParentJob, Errors))
										bFailed = true;
								}
								if (!bFailed)
									bSetOnParent = true;
							}
						}

						NStr::CWStr JobName
							= NStr::CWStr::CFormat(str_utf16("Local\\AnonMalterlibJobObject.{nfh,sf0,sj8}.{nfh,sf0,sj8}.{nfh,sf0,sj8}.{nfh,sf0,sj16}"))
							<< GetCurrentProcessId()
							<< ++g_PipeSerialNumber
							<< NMisc::fg_GetSecureRandomUnsigned()
							<< &g_PipeSerialNumber
						;

						HANDLE Job = CreateJobObjectW(nullptr, JobName);
						auto JobCleanup = fg_OnScopeExit
							(
								[&]()
								{
									if (Job)
										CloseHandle(Job);
								}
							)
						;

						if (Job != NULL)
						{
							if (ParentJob)
							{
								if (!AssignProcessToJobObject(ParentJob, ProcessInfo.hProcess))
								{
									[[maybe_unused]] HRESULT Error = GetLastError();
									DMibDTrace("AssignProcessToJobObject: {}" DMibNewLine, NMib::NPlatform::fg_Win32_GetLastErrorStr(Error));
								}
							}
							if (AssignProcessToJobObject(Job, ProcessInfo.hProcess))
							{
								if (!bSetOnParent)
								{
									if (fg_SetLimitsOnJob(mp_LastLaunchOptions, Job, _Errors))
										bFailedLaunch = true;
								}

								if (!bFailedLaunch && mp_LastLaunchOptions.m_bSandboxed)
								{
									JOBOBJECT_EXTENDED_LIMIT_INFORMATION LimitInfo;
									NMemory::fg_MemClear(LimitInfo);

									DWORD ReturnLength;
									QueryInformationJobObject(Job, JobObjectExtendedLimitInformation, &LimitInfo, sizeof(LimitInfo), &ReturnLength);

									LimitInfo.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;

									BOOL bOK = SetInformationJobObject(Job, JobObjectExtendedLimitInformation, &LimitInfo, sizeof(LimitInfo));
									if (!bOK)
									{
										_Errors += NStr::CStr::CFormat("Failed to set job kill on close flag: {}" DMibNewLine) << NMib::NPlatform::fg_Win32_GetLastErrorStr(GetLastError());
										bFailedLaunch = true;
									}
								}

								if (mp_LastLaunchOptions.m_bSandboxed)
								{
									mp_SandboxJob = Job;
									JobCleanup.f_Clear();
								}
							}
							else
							{
								HRESULT Error = GetLastError();
								DMibDTrace("AssignProcessToJobObject: {}" DMibNewLine, NMib::NPlatform::fg_Win32_GetLastErrorStr(Error));
								_Errors += NStr::CStr::CFormat("Failed to assign job object to process: {}" DMibNewLine) << NMib::NPlatform::fg_Win32_GetLastErrorStr(Error);
								bFailedLaunch = true;
							}
						}
						else
						{
							DWORD Error = GetLastError();
							_Errors += NStr::CStr::CFormat("Failed to create job object: {}" DMibNewLine) << NMib::NPlatform::fg_Win32_GetLastErrorStr(Error);
							bFailedLaunch = true;
						}
					}
				}

#if 0
				if (!bFailedLaunch && mp_LastLaunchOptions.m_LaunchPriority == EExecutionPriority_Lowest)
				{
					static DWORD const DefaultMemoryPriority = 5;
					static DWORD const LowMemoryPriority = 1;
					static DWORD const DefaultIoPriority = 2;
					static DWORD const LowIoPriority = 1;

					typedef struct _MEMORY_PRIORITY_INFORMATION
					{
						ULONG MemoryPriority;
					} MEMORY_PRIORITY_INFORMATION, *PMEMORY_PRIORITY_INFORMATION;

					MEMORY_PRIORITY_INFORMATION MemoryPrio;
					MemoryPrio.MemoryPriority = LowMemoryPriority;

					auto Size = sizeof(MemoryPrio);
					if
					(
						!NLocal::g_OptionalFunctions.m_fSetProcessInformation
						|| !NLocal::g_OptionalFunctions.m_fSetProcessInformation(ProcessInfo.hProcess, ProcessMemoryPriority, &MemoryPrio, sizeof(MemoryPrio))
					)
					{
						DMibTrace("Failed to set memory priority {}\n", NMib::NPlatform::fg_Win32_GetLastErrorStr(GetLastError()));
						if (NLocal::g_OptionalFunctions.m_fNtSetInformationProcess)
						{
							auto Result = NLocal::g_OptionalFunctions.m_fNtSetInformationProcess
								(
									ProcessInfo.hProcess
									, (PROCESSINFOCLASS)ProcessPagePriority
									, &MemoryPrio
									, sizeof(MemoryPrio)
								)
							;
							if (!NT_SUCCESS(Result))
								DMibTrace("Failed to set memory priority {}\n", NMib::NPlatform::fg_Win32_GetLastErrorStr(GetLastError()));
						}
					}

					if (NLocal::g_OptionalFunctions.m_fNtSetInformationProcess)
					{
						ULONG IoPrio = 0;
						auto Result = NLocal::g_OptionalFunctions.m_fNtSetInformationProcess(ProcessInfo.hProcess, (PROCESSINFOCLASS)ProcessIoPriority, &IoPrio, sizeof(IoPrio));
						if (!NT_SUCCESS(Result))
							DMibTrace("Failed to set IO priority {}\n", NMib::NPlatform::fg_Win32_GetLastErrorStr(GetLastError()));
					}
				}
#endif

				if (mp_LastLaunchOptions.m_bSandboxed && !bFailedLaunch && !mp_LastLaunchOptions.m_SandboxRoots.f_IsEmpty())
				{
#ifdef DArchitecture_arm64
					_Errors += "Sandboxing not supported on arm64 Windows" DMibNewLine;
					bFailedLaunch = true;
#else
					NStr::CStr Errors;
#	ifdef DArchitecture_x64
					DWORD NeededType = SCS_64BIT_BINARY;
#	else
					DWORD NeededType = SCS_32BIT_BINARY;
#	endif
					if (BinaryType == NeededType)
					{
						NStr::CWStr WindowsDllName = NFile::NPlatform::fg_ConvertToWindowsPath(SandboxFullPath, false);
						if (!NMib::NPlatform::fg_InjectDLL(ProcessInfo.hProcess, ProcessInfo.hThread, WindowsDllName, Errors))
						{
							_Errors += Errors;
							bFailedLaunch = true;
						}
					}
					else
					{
						// Launch process to do the injection

						NStr::CStr SandboxInjecter = NFile::CFile::fs_GetPath(SandboxFullPath) + "/" + NFile::CFile::fs_GetFileNoExt(SandboxFullPath) + ".exe";

						CProcessLaunchParams LaunchParams = CProcessLaunchParams::fs_LaunchExecutableRawParams
							(
								SandboxInjecter
								, NStr::CStr::CFormat("0x{nfh} {}") << ProcessInfo.dwProcessId << ProcessInfo.dwThreadId
								, NFile::CFile::fs_GetCurrentDirectory()
								, [&](CProcessLaunchStateChangeVariant const &_StateChange, fp64 _TimeSinceLaunch)
								{
									if (_StateChange.f_GetTypeID() == EProcessLaunchState_LaunchFailed)
									{
										_Errors += NStr::CStr::CFormat("Sandbox injection failed with: {}" DMibNewLine) << _StateChange.f_Get<EProcessLaunchState_LaunchFailed>();
										bFailedLaunch = true;
									}
									else if (_StateChange.f_GetTypeID() == EProcessLaunchState_Exited)
									{
										NMib::NPlatform::EInjectDllResult Result = (NMib::NPlatform::EInjectDllResult)_StateChange.f_Get<EProcessLaunchState_Exited>();
										if (Result != NMib::NPlatform::EInjectDllResult_Delayed && Result != NMib::NPlatform::EInjectDllResult_Done)
										{
											_Errors += NStr::CStr::CFormat("Sandbox injection exited with error: {}" DMibNewLine) << _StateChange.f_Get<EProcessLaunchState_Exited>();
											bFailedLaunch = true;
										}
									}
								}
							)
						;

						LaunchParams.m_fOnOutput = [&](EProcessLaunchOutputType _OutputType, NStr::CStr const &_Output)
							{
								switch (_OutputType)
								{
								case EProcessLaunchOutputType_GeneralError:
								case EProcessLaunchOutputType_StdErr:
								case EProcessLaunchOutputType_TerminateMessage:
									_Errors += _Output;
									bFailedLaunch = true;
									break;
								case EProcessLaunchOutputType_StdOut:
									_Errors += _Output;
									bFailedLaunch = true;
									break;
								case EProcessLaunchOutputType_Max: break;
								}
							}
						;
						LaunchParams.m_bSandboxed = false;
						LaunchParams.m_bEnableStdRedirection = true;
						{
							CProcessLaunch Launch(LaunchParams, EProcessLaunchCloseFlag_BlockOnExit);
						}
					}
#endif
				}

				// Without the job the tree is terminated through the parents of the processes, which works as long as they are alive
				if (!bFailedLaunch && mp_LastLaunchOptions.m_bCreateNewProcessGroup && !mp_LastLaunchOptions.m_bSandboxed)
				{
					HANDLE ProcessGroupJob = CreateJobObjectW(nullptr, nullptr);
					if (ProcessGroupJob)
					{
						// Processes that ask to leave the job can, as they would fail to start otherwise
						JOBOBJECT_EXTENDED_LIMIT_INFORMATION LimitInfo;
						NMemory::fg_MemClear(LimitInfo);
						LimitInfo.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_BREAKAWAY_OK;

						if
						(
							SetInformationJobObject(ProcessGroupJob, JobObjectExtendedLimitInformation, &LimitInfo, sizeof(LimitInfo))
							&& AssignProcessToJobObject(ProcessGroupJob, ProcessInfo.hProcess)
						)
						{
							fp_DestroyHandle(mp_ProcessGroupJob);
							mp_ProcessGroupJob = ProcessGroupJob;
						}
						else
							CloseHandle(ProcessGroupJob);
					}
				}

				if (!bFailedLaunch && (CreateProcessFlags & CREATE_SUSPENDED))
					ResumeThread(ProcessInfo.hThread);

				if (!bFailedLaunch)
				{
					// The unload decision at the child's exit needs the session-end listener running by then
					if (!CleanupUserProfiles.f_IsEmpty())
						NMib::NPlatform::fg_EnsureEndSessionReporting();

					mp_CleanupLoadedProfiles = fg_Move(CleanupUserProfiles);
					mp_ChildProcess = ProcessInfo.hProcess;
					mp_ProcessID = ProcessInfo.dwProcessId;
				}
				else
				{
					TerminateProcess(ProcessInfo.hProcess, 255);
					::CloseHandle(ProcessInfo.hProcess);
					::CloseHandle(ProcessInfo.hThread);
					return false;
				}
				// Close any unuseful handles
				::CloseHandle(ProcessInfo.hThread);
				return true;
			}
			return false;
		}

		void WINAPI CConsoleRedirector::fsp_StdOutReadFinished(DWORD _ErrorCode, DWORD _nBytesTransferred, LPOVERLAPPED _pOverlapped)
		{
			CConsoleRedirector *pThis = (CConsoleRedirector *)_pOverlapped->hEvent;
			if (_nBytesTransferred)
				pThis->fp_OnStdOutput(NMib::NProcess::EProcessLaunchOutputType_StdOut, (uint8 const *)_pOverlapped->Pointer, _nBytesTransferred);

			if
			(
				_ErrorCode == ERROR_OPERATION_ABORTED // CancelIo was called
				|| _ErrorCode == ERROR_BROKEN_PIPE // Pipe has been ended
				|| _ErrorCode == ERROR_NO_DATA // Pipe closing in progress
			)
			{
				return;
			}

			if (_ErrorCode)
			{
				NStr::CStr Error = NMib::NPlatform::fg_Win32_GetLastErrorStr(_ErrorCode);
				pThis->fp_OnOutput(NMib::NProcess::EProcessLaunchOutputType_GeneralError, NStr::CStr::CFormat("Read stdout pipe error: {}" DMibNewLine) << Error);
			}

			if (::ReadFileEx(pThis->mp_StdoutRead, pThis->mp_StdOutReadBuffer.f_GetArray(), 4096, &pThis->mp_StdOutRead, &CConsoleRedirector::fsp_StdOutReadFinished))
				return;

			_ErrorCode = GetLastError();
			if
			(
				_ErrorCode == ERROR_OPERATION_ABORTED // CancelIo was called
				|| _ErrorCode == ERROR_BROKEN_PIPE // Pipe has been ended
				|| _ErrorCode == ERROR_NO_DATA // Pipe closing in progress
			)
			{
				return;
			}

			NStr::CStr Error = NMib::NPlatform::fg_Win32_GetLastErrorStr(_ErrorCode);
			pThis->fp_OnOutput(NMib::NProcess::EProcessLaunchOutputType_GeneralError, NStr::CStr::CFormat("Read stdout pipe error: {}" DMibNewLine) << Error);
		}

		void WINAPI CConsoleRedirector::fsp_StdErrReadFinished(DWORD _ErrorCode, DWORD _nBytesTransferred, LPOVERLAPPED _pOverlapped)
		{
			CConsoleRedirector *pThis = (CConsoleRedirector *)_pOverlapped->hEvent;
			if (_nBytesTransferred)
				pThis->fp_OnStdOutput(NMib::NProcess::EProcessLaunchOutputType_StdErr, (uint8 const *)_pOverlapped->Pointer, _nBytesTransferred);

			if
			(
				_ErrorCode == ERROR_OPERATION_ABORTED // CancelIo was called
				|| _ErrorCode == ERROR_BROKEN_PIPE // Pipe has been ended
				|| _ErrorCode == ERROR_NO_DATA // Pipe closing in progress
			)
			{
				return;
			}

			if (_ErrorCode)
			{
				NStr::CStr Error = NMib::NPlatform::fg_Win32_GetLastErrorStr(_ErrorCode);
				pThis->fp_OnOutput(NMib::NProcess::EProcessLaunchOutputType_GeneralError, NStr::CStr::CFormat("Read stderr pipe error: {}" DMibNewLine) << Error);
			}

			if (::ReadFileEx(pThis->mp_StderrRead, pThis->mp_StdErrReadBuffer.f_GetArray(), 4096, &pThis->mp_StdErrRead, &CConsoleRedirector::fsp_StdErrReadFinished))
				return;

			_ErrorCode = GetLastError();
			if
			(
				_ErrorCode == ERROR_OPERATION_ABORTED // CancelIo was called
				|| _ErrorCode == ERROR_BROKEN_PIPE // Pipe has been ended
				|| _ErrorCode == ERROR_NO_DATA // Pipe closing in progress
			)
			{
				return;
			}

			NStr::CStr Error = NMib::NPlatform::fg_Win32_GetLastErrorStr(_ErrorCode);
			pThis->fp_OnOutput(NMib::NProcess::EProcessLaunchOutputType_GeneralError, NStr::CStr::CFormat("Read stderr pipe error: {}" DMibNewLine) << Error);
		}

		int CConsoleRedirector::fp_RedirectStdout()
		{
			DMibSafeCheck(mp_StdoutRead != nullptr, "");

			if (mp_StdOutReadBuffer.f_IsEmpty())
			{
				mp_StdOutReadBuffer.f_SetLen(4096);
				NMemory::fg_MemClear(mp_StdOutRead);
				mp_StdOutRead.hEvent = (void *)this;
				mp_StdOutRead.Pointer = mp_StdOutReadBuffer.f_GetArray();
			}

			fsp_StdOutReadFinished(0, 0, &mp_StdOutRead);

			return 0;
		}

		int CConsoleRedirector::fp_RedirectStderr()
		{
			DMibSafeCheck(mp_StderrRead != nullptr, "");

			if (mp_StdErrReadBuffer.f_IsEmpty())
			{
				mp_StdErrReadBuffer.f_SetLen(4096);
				NMemory::fg_MemClear(mp_StdErrRead);
				mp_StdErrRead.hEvent = (void *)this;
				mp_StdErrRead.Pointer = mp_StdErrReadBuffer.f_GetArray();
			}

			fsp_StdErrReadFinished(0, 0, &mp_StdOutRead);

			return 0;
		}

		void CConsoleRedirector::fp_DestroyHandle(HANDLE &_Handle)
		{
			if (_Handle != nullptr)
			{
				::CloseHandle(_Handle);
				_Handle = nullptr;
			}
		}

		aint CConsoleRedirector::f_Main()
		{
			NStr::CStr Errors;
			if (!fp_DoStart(Errors))
			{
				fp_OnLaunched(Errors, mp_ChildProcess, false);
				return 0;
			}
			fp_OnLaunched(Errors, mp_ChildProcess, true);

			HANDLE WaitForHandles[4];
			WaitForHandles[0] = mp_ChildProcess;
			WaitForHandles[1] = mp_Event;

			bool bExited = false;

			// Kickstart IO
			if (mp_StdoutRead)
				fp_RedirectStdout();
			if (mp_StderrRead)
				fp_RedirectStderr();

			while (true)
			{
				DWORD Object = WaitForMultipleObjectsEx(2, WaitForHandles, false, 10000, true);

				if (Object == WAIT_IO_COMPLETION)
				{
				}
				else if (Object == WAIT_OBJECT_0 || mp_ChildProcess == nullptr)
				{
					bExited = true;
					if (!mp_CleanupLoadedProfiles.f_IsEmpty())
					{
						if (NMib::NPlatform::fg_IsShuttingDown())
						{
							// When shutting down unloading a profile will hang for several minutes, instead leak the profiles
							for (auto &pProfile : mp_CleanupLoadedProfiles)
								pProfile->f_Clear();
						}
						mp_CleanupLoadedProfiles.f_Clear();
					}
					break;
				}
				else if (Object == WAIT_OBJECT_0 + 1)
				{
					NMib::NProcess::EProcessLaunchCloseFlag NeedTermination = NMib::NProcess::EProcessLaunchCloseFlag_None;
					bool bNeedWait = false;
					{
						DMibLock(mp_NeedTerminationLock);
						fg_Swap(NeedTermination, mp_NeedTermination);
						fg_Swap(bNeedWait, mp_bNeedWait);
					}

					if (NeedTermination & (NMib::NProcess::EProcessLaunchCloseFlag_TerminateProcess | NMib::NProcess::EProcessLaunchCloseFlag_TerminateProcessTree))
					{
						NStr::CStr TempRet;
						if (!mp_LastLaunchOptions.m_bSandboxed) // If we are sandboxed just break and let the sandbox program kill all processes in sandbox
						{
							f_TerminateTree(TempRet);
						}
						else
							fp_TerminateSandbox(TempRet);
						if (!TempRet.f_IsEmpty())
							fp_OnOutput(NMib::NProcess::EProcessLaunchOutputType_TerminateMessage, TempRet);
					}
					else if (NeedTermination & NMib::NProcess::EProcessLaunchCloseFlag_StopProcess)
					{
						NStr::CStr TempRet;
						if (mp_ProcessID)
						{
							try
							{
								f_StopProcess();
							}
							catch (NException::CException const &_Exception)
							{
								TempRet = _Exception.f_GetErrorStr();
								bNeedWait = false;
							}
						}
						else
						{
							fp_OnOutput(NMib::NProcess::EProcessLaunchOutputType_TerminateMessage, "Cannot stop process, because no process ID is available");
							bNeedWait = false;
						}
						if (!TempRet.f_IsEmpty())
							fp_OnOutput(NMib::NProcess::EProcessLaunchOutputType_TerminateMessage, TempRet);
					}
					if (!bNeedWait)
					{
						if (!mp_CleanupLoadedProfiles.f_IsEmpty())
						{
							for (auto &pProfile : mp_CleanupLoadedProfiles)
								pProfile->f_Clear(); // Process still running, let the profile leak
							mp_CleanupLoadedProfiles.f_Clear();
						}
						break;
					}
				}
			}

			// Read any outstading IO
			while (SleepEx(0, true) == WAIT_IO_COMPLETION)
				;

			// Cancel IO
			CancelIo(mp_StdoutRead);
			if (mp_StderrRead)
				CancelIo(mp_StderrRead);

			// Wait for cancellation
			while (SleepEx(0, true) == WAIT_IO_COMPLETION)
				;

			if (bExited)
			{
				fp_UpdateOverall();
				if (mp_LastLaunchOptions.m_fOnStateChange)
				{
					DWORD ExitCode = 255;
					if (mp_ChildProcess)
						GetExitCodeProcess(mp_ChildProcess, &ExitCode);
					fp_OnExit(ExitCode);
				}
			}

			if (bExited)
				fp_ClearSandbox();

			fp_Close();
			return 0;
		}

		void CConsoleRedirector::fp_OnLaunched(NMib::NStr::CStr const &_Error, void *_pProcess, bool _bSuccess)
		{
			fp64 TimeSinceStart;
			{
				DMibLock(mp_ExitTimeLock);
				TimeSinceStart = mp_TimeSinceStart.f_GetTime();
				if (!_pProcess)
					mp_ExitTime = TimeSinceStart;
			}
			if (mp_LastLaunchOptions.m_bAllowLaunchedInForeground && _pProcess)
				AllowSetForegroundWindow(GetProcessId(_pProcess));
			if (mp_LastLaunchOptions.m_fOnStateChange)
			{
				if (mp_LastLaunchOptions.m_fDispatcher)
				{
					NStorage::TCSharedPointer<CConsoleRedirector> pThis = fg_Explicit(this);
					mp_LastLaunchOptions.m_fDispatcher
						(
							[_Error, _pProcess, pThis, TimeSinceStart, _bSuccess]()
							{
								if (_bSuccess)
									pThis->mp_LastLaunchOptions.m_fOnStateChange(_pProcess, TimeSinceStart);
								else
									pThis->mp_LastLaunchOptions.m_fOnStateChange(_Error, TimeSinceStart);
							}
						)
					;
				}
				else
				{
					if (_bSuccess)
						mp_LastLaunchOptions.m_fOnStateChange(_pProcess, TimeSinceStart);
					else
						mp_LastLaunchOptions.m_fOnStateChange(_Error, TimeSinceStart);
				}
			}
		}

		void CConsoleRedirector::fp_OnOutput(NMib::NProcess::EProcessLaunchOutputType _OutputType, NMib::NStr::CStr const &_Output)
		{
			if (mp_LastLaunchOptions.m_fOnOutput)
			{
				if (mp_LastLaunchOptions.m_fDispatcher)
				{
					NStorage::TCSharedPointer<CConsoleRedirector> pThis = fg_Explicit(this);
					mp_LastLaunchOptions.m_fDispatcher
						(
							[_OutputType, _Output, pThis]()
							{
								pThis->mp_LastLaunchOptions.m_fOnOutput(_OutputType, _Output);
							}
						)
					;
				}
				else
					mp_LastLaunchOptions.m_fOnOutput(_OutputType, _Output);
			}
		}

		// Stdout and stderr as read: bytes for m_fOnOutputBinary, otherwise text assumed to be UTF-8
		void CConsoleRedirector::fp_OnStdOutput(NMib::NProcess::EProcessLaunchOutputType _OutputType, uint8 const *_pData, umint _Size)
		{
			if (!mp_LastLaunchOptions.m_fOnOutputBinary)
			{
				fp_OnOutput(_OutputType, NStr::CStr((ch8 const *)_pData, _Size));
				return;
			}

			NContainer::CIOByteVector Output;
			Output.f_Insert(_pData, _Size);

			if (mp_LastLaunchOptions.m_fDispatcher)
			{
				NStorage::TCSharedPointer<CConsoleRedirector> pThis = fg_Explicit(this);
				mp_LastLaunchOptions.m_fDispatcher
					(
						[_OutputType, Output = fg_Move(Output), pThis]()
						{
							pThis->mp_LastLaunchOptions.m_fOnOutputBinary(_OutputType, Output);
						}
					)
				;
			}
			else
				mp_LastLaunchOptions.m_fOnOutputBinary(_OutputType, Output);
		}

		void CConsoleRedirector::fp_OnExit(uint32 _ExitCode)
		{
			fp64 TimeSinceStart;
			{
				DMibLock(mp_ExitTimeLock);
				TimeSinceStart = mp_TimeSinceStart.f_GetTime();
				mp_ExitTime = TimeSinceStart;
			}

			if (mp_LastLaunchOptions.m_fOnStateChange)
			{
				if (mp_LastLaunchOptions.m_fDispatcher)
				{
					NStorage::TCSharedPointer<CConsoleRedirector> pThis = fg_Explicit(this);
					mp_LastLaunchOptions.m_fDispatcher
						(
							[pThis, _ExitCode, TimeSinceStart]()
							{
								pThis->mp_LastLaunchOptions.m_fOnStateChange(_ExitCode, TimeSinceStart);
							}
						)
					;
				}
				else
					mp_LastLaunchOptions.m_fOnStateChange(_ExitCode, TimeSinceStart);
			}
		}

		static BOOL APIENTRY fsg_CreatePipeEx
			(
				OUT LPHANDLE o_pReadPipe
				, OUT LPHANDLE o_pWritePipe
				, IN LPSECURITY_ATTRIBUTES _pPipeAttributes
				, IN DWORD _BufferSize
				, DWORD _ReadMode
				, DWORD _WriteMode
				, NMib::NStr::CStr &_ExtendedError
			)
		/*++

		Routine Description:

			The CreatePipeEx API is used to create an anonymous pipe I/O device.
			Unlike CreatePipe FILE_FLAG_OVERLAPPED may be specified for one or
			both handles.
			Two handles to the device are created.  One handle is opened for
			reading and the other is opened for writing.  These handles may be
			used in subsequent calls to ReadFile and WriteFile to transmit data
			through the pipe.

		Arguments:

			o_pReadPipe - Returns a handle to the read side of the pipe.  Data
				may be read from the pipe by specifying this handle value in a
				subsequent call to ReadFile.

			o_pWritePipe - Returns a handle to the write side of the pipe.  Data
				may be written to the pipe by specifying this handle value in a
				subsequent call to WriteFile.

			_pPipeAttributes - An optional parameter that may be used to specify
				the attributes of the new pipe.  If the parameter is not
				specified, then the pipe is created without a security
				descriptor, and the resulting handles are not inherited on
				process creation.  Otherwise, the optional security attributes
				are used on the pipe, and the inherit handles flag effects both
				pipe handles.

			_BufferSize - Supplies the requested buffer size for the pipe.  This is
				only a suggestion and is used by the operating system to
				calculate an appropriate buffering mechanism.  A value of zero
				indicates that the system is to choose the default buffering
				scheme.

		Return Value:

			TRUE - The operation was successful.

			FALSE/NULL - The operation failed. Extended error status is available
				using GetLastError.

		--*/

		{
			HANDLE ReadPipeHandle, WritePipeHandle;
			DWORD ErrorCode;

			//
			// Only one valid OpenMode flag - FILE_FLAG_OVERLAPPED
			//

			if ((_ReadMode | _WriteMode) & (~FILE_FLAG_OVERLAPPED))
			{
				SetLastError(ERROR_INVALID_PARAMETER);
				return FALSE;
			}

			//
			// Set the default timeout to 120 seconds
			//

			if (_BufferSize == 0)
				_BufferSize = 4096;

			NStr::CWStr PipeName
				= NStr::CWStr::CFormat(str_utf16("\\\\.\\Pipe\\AnonOverlapped.{nfh,sf0,sj8}.{nfh,sf0,sj8}.{nfh,sf0,sj8}.{nfh,sf0,sj16}"))
				<< GetCurrentProcessId()
				<< ++g_PipeSerialNumber
				<< NMisc::fg_GetSecureRandomUnsigned()
				<< &g_PipeSerialNumber
			;

			bool bRetry = true;
		Retry:
			ReadPipeHandle = CreateNamedPipeW
				(
					PipeName.f_GetStr()
					, PIPE_ACCESS_INBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE | _ReadMode
					, PIPE_TYPE_BYTE | PIPE_WAIT
					, 1 // Number of pipes
					, _BufferSize // Out buffer size
					, _BufferSize // In buffer size
					, 120 * 1000 // Timeout in ms
					, _pPipeAttributes
				)
			;

			if (!ReadPipeHandle || ReadPipeHandle == INVALID_HANDLE_VALUE)
			{
				if (GetLastError() == ERROR_PIPE_BUSY)
				{
					if (bRetry)
					{
						bRetry = false;
						NTime::CStopwatch Time;
						Time.f_Start();
						while (!WaitNamedPipe(PipeName, 20000))
						{
							if (GetLastError() == ERROR_SEM_TIMEOUT)
							{
								_ExtendedError = "Timed out creating pipe";
								SetLastError(ERROR_PIPE_BUSY);
								return false;
							}
							Sleep(1);
							if (Time.f_GetTime() > 20.0)
							{
								_ExtendedError = "Timed out creating pipe 2";
								SetLastError(ERROR_PIPE_BUSY);
								return false;
							}
						}
						goto Retry;
					}
					_ExtendedError = "No more retries creating pipe";
					SetLastError(ERROR_PIPE_BUSY);
				}
				return FALSE;
			}

			auto Cleanup = fg_OnScopeExit
				(
					[&]
					{
						auto OldError = GetLastError();
						CloseHandle(ReadPipeHandle);
						SetLastError(OldError);
					}
				)
			;

			bool bRetry2 = true;
		Retry2:

			WritePipeHandle = CreateFileW
				(
					PipeName.f_GetStr()
					, GENERIC_WRITE
					, 0 // No sharing
					, _pPipeAttributes
					, OPEN_EXISTING
					, FILE_ATTRIBUTE_NORMAL | _WriteMode
					, NULL // Template file
				)
			;

			if (!WritePipeHandle || INVALID_HANDLE_VALUE == WritePipeHandle)
			{
				ErrorCode = GetLastError();
				if (ErrorCode == ERROR_PIPE_BUSY)
				{
					if (bRetry2)
					{
						bRetry2 = false;
						NTime::CStopwatch Time;
						Time.f_Start();
						while (!WaitNamedPipe(PipeName, 20000))
						{
							if (GetLastError() == ERROR_SEM_TIMEOUT)
							{
								_ExtendedError = "Timed out connecting to pipe";
								SetLastError(ERROR_PIPE_BUSY);
								return false;
							}
							Sleep(1);
							if (Time.f_GetTime() > 20.0)
							{
								_ExtendedError = "Timed out connecting to pipe 2";
								SetLastError(ERROR_PIPE_BUSY);
								return false;
							}
						}
						goto Retry2;
					}
					_ExtendedError = "No more retries creating pipe";
				}

				SetLastError(ErrorCode);
				return FALSE;
			}

			*o_pReadPipe = ReadPipeHandle;
			*o_pWritePipe = WritePipeHandle;
			Cleanup.f_Clear();
			return TRUE;
		}

		bool CConsoleRedirector::fp_DoStart(NStr::CStr &_Errors)
		{
			HANDLE StdoutReadTemp; // parent stdout read handle
			HANDLE StdoutWrite; // child stdout write handle
			HANDLE StderrReadTemp; // parent stdout read handle
			HANDLE StderrWrite; // child stdout write handle
			HANDLE StdinWriteTemp; // parent stdin write handle
			HANDLE StdinRead; // child stdin read handle

			fp_Close();
			StdoutReadTemp = nullptr;
			StdoutWrite = nullptr;
			StderrReadTemp = nullptr;
			StderrWrite = nullptr;
			StdinWriteTemp = nullptr;
			StdinRead = nullptr;

			// Set up the security attributes struct.
			SECURITY_ATTRIBUTES SecurityAttributes;
			SecurityAttributes.nLength = sizeof(SECURITY_ATTRIBUTES);
			SecurityAttributes.lpSecurityDescriptor = nullptr;
			SecurityAttributes.bInheritHandle = TRUE;

			// For elevated launches with stdio redirection, named pipes are created
			// in fp_LaunchChild instead of anonymous pipes here, because
			// ShellExecuteExW with "runas" does not support handle inheritance.
			bool bElevatedStdio
				= mp_LastLaunchOptions.m_bEnableStdRedirection && mp_LastLaunchOptions.m_bStdOutPID && mp_LastLaunchOptions.m_Elevation == NMib::NProcess::EProcessLaunchElevation_Elevate
			;

			BOOL bOK = false;
			do
			{
				// Create a child stdout pipe.
				{
					if (mp_LastLaunchOptions.m_bEnableStdRedirection && !bElevatedStdio)
					{
						NStr::CStr ExtendedError;
						if (!fsg_CreatePipeEx(&StdoutReadTemp, &StdoutWrite, &SecurityAttributes, 0, FILE_FLAG_OVERLAPPED, 0, ExtendedError))
						{
							DWORD OsError = ::GetLastError();
							_Errors += NStr::CStr::CFormat("CreatePipe (StdOut) failed: {} {}" DMibNewLine) << NMib::NPlatform::fg_Win32_GetLastErrorStr(OsError) << ExtendedError;
							break;
						}

						if (mp_LastLaunchOptions.m_bSeparateStdErr)
						{
							if (!fsg_CreatePipeEx(&StderrReadTemp, &StderrWrite, &SecurityAttributes, 0, FILE_FLAG_OVERLAPPED, 0, ExtendedError))
							{
								DWORD OsError = ::GetLastError();
								_Errors += NStr::CStr::CFormat("CreatePipe (StdErr) failed: {} {}" DMibNewLine) << NMib::NPlatform::fg_Win32_GetLastErrorStr(OsError) << ExtendedError;
								break;
							}
						}

						// Create a child stdin pipe.
						if (!fsg_CreatePipeEx(&StdinRead, &StdinWriteTemp, &SecurityAttributes, 0, 0, 0, ExtendedError))
						{
							DWORD OsError = ::GetLastError();
							_Errors += NStr::CStr::CFormat("CreatePipe (StdIn) failed: {} {}" DMibNewLine) << NMib::NPlatform::fg_Win32_GetLastErrorStr(OsError) << ExtendedError;
							break;
						}

						// Create new stdout read handle and the stdin write handle.
						// Set the inheritance properties to FALSE. Otherwise, the child
						// inherits the these handles; resulting in non-closeable
						// handles to the pipes being created.
						if
						(
							!::DuplicateHandle
							(
								::GetCurrentProcess()
								, StdoutReadTemp
								, ::GetCurrentProcess()
								, &mp_StdoutRead
								, 0
								, FALSE // make it uninheritable.
								, DUPLICATE_SAME_ACCESS
							)
						)
						{
							DWORD OsError = ::GetLastError();
							_Errors += NStr::CStr::CFormat("DuplicateHandle failed: {}" DMibNewLine) << NMib::NPlatform::fg_Win32_GetLastErrorStr(OsError);
							break;
						}

						if (mp_LastLaunchOptions.m_bSeparateStdErr)
						{
							if
							(
								!::DuplicateHandle
								(
									::GetCurrentProcess()
									, StderrReadTemp
									, ::GetCurrentProcess()
									, &mp_StderrRead
									, 0
									, FALSE // make it uninheritable.
									, DUPLICATE_SAME_ACCESS
								)
							)
							{
								DWORD OsError = ::GetLastError();
								_Errors += NStr::CStr::CFormat("DuplicateHandle failed: {}" DMibNewLine) << NMib::NPlatform::fg_Win32_GetLastErrorStr(OsError);
								break;
							}
						}

						{
							DMibLock(mp_PipeLock);

							if
							(
								!::DuplicateHandle
								(
									::GetCurrentProcess()
									, StdinWriteTemp
									, ::GetCurrentProcess()
									, &mp_StdinWrite
									, 0
									, FALSE // make it uninheritable.
									, DUPLICATE_SAME_ACCESS
								)
							)
							{
								DWORD OsError = ::GetLastError();
								_Errors += NStr::CStr::CFormat("DuplicateHandle failed: {}" DMibNewLine) << NMib::NPlatform::fg_Win32_GetLastErrorStr(OsError);
								break;
							}
						}

						// Close inheritable copies of the handles we do not want to
						// be inherited.
						fp_DestroyHandle(StdoutReadTemp);
						fp_DestroyHandle(StderrReadTemp);
						fp_DestroyHandle(StdinWriteTemp);
					}
				}

				// launch the child process
				if (!fp_LaunchChild(StdoutWrite, StdinRead, mp_LastLaunchOptions.m_bSeparateStdErr ? StderrWrite : StdoutWrite, _Errors))
					break;

				// Child is launched. Close the parents copy of those pipe
				// handles that only the child should have open.
				// Make sure that no handles to the write end of the stdout pipe
				// are maintained in this process or else the pipe will not
				// close when the child process exits and ReadFile will hang.
				fp_DestroyHandle(StdoutWrite);
				fp_DestroyHandle(StderrWrite);
				fp_DestroyHandle(StdinRead);

				bOK = TRUE;
			}
			while (0);

			if (!bOK)
			{
				fp_DestroyHandle(StdoutReadTemp);
				fp_DestroyHandle(StdoutWrite);
				fp_DestroyHandle(StderrReadTemp);
				fp_DestroyHandle(StderrWrite);
				fp_DestroyHandle(StdinWriteTemp);
				fp_DestroyHandle(StdinRead);
				fp_Close();
			}

			return bOK;
		}

		bool CConsoleRedirector::f_Open(NMib::NProcess::CProcessLaunchParams const &_Options)
		{
			mp_LastLaunchOptions = _Options;
			return true;
		}

		bool CConsoleRedirector::f_StartRedirector(EProcessLaunchCloseFlag _Flags)
		{
			if (mp_bStarted)
				DMibError(NStr::CStrNonTracked("Launch has already been started"));
			mp_bStarted = true;
			// Increase ref count for thread
			if (mp_LastLaunchOptions.m_bThreaded)
			{
				m_RefCount.f_Increase(DIfRefCountDebugging(m_DebugSelfThreadRef));
				auto CleanupRef = fg_OnScopeExit
					(
						[&]()
						{
							m_RefCount.f_Decrease(DIfRefCountDebugging(m_DebugSelfThreadRef));
						}
					)
				;

				__super::f_Start(EExecutionPriority_Highest, 0, 0, EThreadCreateFlag_AutoDestroy);

				CleanupRef.f_Clear();
			}
			else
			{
				{
					DMibLock(mp_NeedTerminationLock);
					mp_NeedTermination
						= _Flags
						&
						(
							NMib::NProcess::EProcessLaunchCloseFlag_TerminateProcess
							| NMib::NProcess::EProcessLaunchCloseFlag_TerminateProcessTree
							| NMib::NProcess::EProcessLaunchCloseFlag_StopProcess
						)
					;
					if (_Flags & (NMib::NProcess::EProcessLaunchCloseFlag_LingerUntilDone | NMib::NProcess::EProcessLaunchCloseFlag_BlockOnExit))
						mp_bNeedWait = true;
				}

				SetEvent(mp_Event);

				f_Main();
			}
			return true;
		}

		void CConsoleRedirector::fp_Close()
		{
			DMibLock(mp_PipeLock);
			{
				DMibLock(mp_ChildProcessLock);
				fp_DestroyHandle(mp_ChildProcess);
			}
			fp_DestroyHandle(mp_StdinWrite);
			fp_DestroyHandle(mp_StdoutRead);
			fp_DestroyHandle(mp_StderrRead);
			fp_DestroyHandle(mp_SandboxJob);
		}

		NMib::NProcess::CProcessStatistics CConsoleRedirector::f_GetOverallExecutionStatistics()
		{
			DMibLock(mp_OverallLock);

			if (!mp_OverallStats_Execution)
				return {};

			auto &Stats = *mp_OverallStats_Execution;

			NTime::CTime CreateTime = NFile::NPlatform::fg_Win32_FileTimeToMalterlibTime(Stats.m_CreateTime);
			NTime::CTime ExitTime = NFile::NPlatform::fg_Win32_FileTimeToMalterlibTime(Stats.m_ExitTime);
			NTime::CTimeSpan KernelTime = NFile::NPlatform::fg_Win32_FileTimeToMalterlibTimeSpan(Stats.m_KernelTime);
			NTime::CTimeSpan UserTime = NFile::NPlatform::fg_Win32_FileTimeToMalterlibTimeSpan(Stats.m_UserTime);
			NTime::CTimeSpan RunTime = ExitTime - CreateTime;

			fp64 KernelSeconds = KernelTime.f_GetSecondsFraction();
			fp64 UserSeconds = UserTime.f_GetSecondsFraction();
			fp64 RunSeconds = RunTime.f_GetSecondsFraction();

			CProcessStatistics Return;
			Return.m_Statistics("CPU utilization Total", CProcessStat(EProcessStatUnit_Fraction, (KernelSeconds + UserSeconds) / RunSeconds));
			Return.m_Statistics("CPU utilization User", CProcessStat(EProcessStatUnit_Fraction, UserSeconds / RunSeconds));
			Return.m_Statistics("CPU utilization Kernel", CProcessStat(EProcessStatUnit_Fraction, KernelSeconds / RunSeconds));

			return Return;
		}

		NMib::NProcess::CProcessStatistics CConsoleRedirector::f_GetOverallMemoryStatistics()
		{
			DMibLock(mp_OverallLock);

			if (!mp_OverallStats_Memory)
				return {};

			auto &Stats = *mp_OverallStats_Memory;

			CProcessStatistics Return;
			Return.m_Statistics("Total page faults", CProcessStat(EProcessStatUnit_GeneralNumber, Stats.PageFaultCount, 1.0, "faults"));
			Return.m_Statistics("Peak working set size", CProcessStat(EProcessStatUnit_Bytes, Stats.PeakWorkingSetSize, 1024 * 1024));
			Return.m_Statistics("Peak paged pool usage", CProcessStat(EProcessStatUnit_Bytes, Stats.QuotaPeakPagedPoolUsage, 1024));
			Return.m_Statistics("Peak non paged pool usage", CProcessStat(EProcessStatUnit_Bytes, Stats.QuotaPeakNonPagedPoolUsage, 1024));
			Return.m_Statistics("Peak page file usage", CProcessStat(EProcessStatUnit_Bytes, Stats.PeakPagefileUsage, 1024 * 1024));

			return Return;
		}

		void CConsoleRedirector::fp_UpdateOverall()
		{
			DMibLock(mp_OverallLock);

			if (!mp_ChildProcess)
				return;

			{
				COverallExecution OverallExecution;
				if (GetProcessTimes(mp_ChildProcess, &OverallExecution.m_CreateTime, &OverallExecution.m_ExitTime, &OverallExecution.m_KernelTime, &OverallExecution.m_UserTime))
					mp_OverallStats_Execution = OverallExecution;
			}

			{
				PROCESS_MEMORY_COUNTERS_EX MemoryInfo;
				NMemory::fg_MemClear(MemoryInfo);

				if (GetProcessMemoryInfo(mp_ChildProcess, (PROCESS_MEMORY_COUNTERS *)&MemoryInfo, sizeof(MemoryInfo)))
					mp_OverallStats_Memory = MemoryInfo;
			}
		}

		bool CConsoleRedirector::f_DestroyThread()
		{
			if (m_RefCount.f_Decrease(DIfRefCountDebugging(m_DebugSelfThreadRef)) == 0)
			{
				fg_DeleteObject(NMemory::CDefaultAllocator(), this);
				return true;
			}
			return false;
		}

		void CConsoleRedirector::f_Cancel()
		{
			{
				DMibLock(mp_NeedTerminationLock);
				mp_bNeedWait = false;
			}
			SetEvent(mp_Event);
		}

		fp64 CConsoleRedirector::f_GetRunningTime()
		{
			DMibLock(mp_ExitTimeLock);
			if (f_IsRunning())
				return mp_TimeSinceStart.f_GetTime();
			else
				return mp_ExitTime;
		}

		umint CConsoleRedirector::f_GetID() const
		{
			return mp_ProcessID;
		}

		void CConsoleRedirector::f_Close(NMib::NProcess::EProcessLaunchCloseFlag _Flags)
		{
			bool bNeedWait;
			{
				{
					DMibLock(mp_NeedTerminationLock);
					mp_NeedTermination
						= _Flags
						&
						(
							NMib::NProcess::EProcessLaunchCloseFlag_TerminateProcess
							| NMib::NProcess::EProcessLaunchCloseFlag_TerminateProcessTree
							| NMib::NProcess::EProcessLaunchCloseFlag_StopProcess
						)
					;
					if (_Flags & (NMib::NProcess::EProcessLaunchCloseFlag_LingerUntilDone | NMib::NProcess::EProcessLaunchCloseFlag_BlockOnExit))
						mp_bNeedWait = true;
					bNeedWait = mp_bNeedWait;
				}
				SetEvent(mp_Event);
			}

			// The launch thread can be done with a process that has exited while descendants live on in its job
			if ((_Flags & NMib::NProcess::EProcessLaunchCloseFlag_TerminateProcessTree) && mp_ProcessGroupJob)
				TerminateJobObject(mp_ProcessGroupJob, 255);

			if (_Flags & NMib::NProcess::EProcessLaunchCloseFlag_BlockOnExit || !bNeedWait)
				f_Stop(true);
		}

		bool CConsoleRedirector::f_IsRunning()
		{
			return (f_GetState() == NThread::EThreadState_Running);
		}

		bool CConsoleRedirector::f_SendText(NStr::CStrIO const &_Data)
		{
			DMibLock(mp_PipeLock);
			if (!mp_StdinWrite)
				return FALSE;

			if (::GetFileType(mp_StdinWrite) == FILE_TYPE_CHAR)
			{
				DWORD nWritten;
				NStr::CWStrIO Output = _Data;
				return ::WriteConsoleW(mp_StdinWrite, Output.f_GetStr(), Output.f_GetLen(), &nWritten, nullptr);
			}
			else
			{
				DWORD nWritten;
				NStr::CStrIO Output = _Data;
				return ::WriteFile(mp_StdinWrite, Output.f_GetStr(), Output.f_GetLen(), &nWritten, nullptr);
			}
		}

		void CConsoleRedirector::f_CloseStdIn()
		{
			DMibLock(mp_PipeLock);
			if (!mp_StdinWrite)
				return;

			fp_DestroyHandle(mp_StdinWrite);
		}

		void CConsoleRedirector::f_SendBinary(NContainer::CIOByteVector const &_Data)
		{
			DMibLock(mp_PipeLock);
			if (!mp_StdinWrite)
				return;
			DWORD nWritten;
			::WriteFile(mp_StdinWrite, _Data.f_GetArray(), _Data.f_GetLen(), &nWritten, nullptr);
		}

		NMib::NProcess::EProcessElevation fg_Process_GetElevation(void *_pProcess)
		{
			using namespace NMib::NSys;
			if (!NMib::NPlatform::fg_IsVista())
				return NMib::NProcess::EProcessElevation_None;

			HANDLE Token = nullptr;

			if (!::OpenProcessToken(_pProcess, TOKEN_QUERY, &Token))
				return NMib::NProcess::EProcessElevation_None;

			auto TokenCleanup = g_OnScopeExit / [&]
				{
					::CloseHandle(Token);
				}
			;

			DWORD ReturnLength = 0;

			TOKEN_ELEVATION_TYPE ElevationType;
			if (!::GetTokenInformation(Token, TokenElevationType, &ElevationType, sizeof(ElevationType), &ReturnLength))
				ElevationType = (TOKEN_ELEVATION_TYPE)0;
			else
				DMibCheck(ReturnLength == sizeof(ElevationType));

			switch (ElevationType)
			{
			default:
			case TokenElevationTypeDefault:
				{
					// TokenElevationTypeDefault means UAC is not involved (e.g. services
					// running as SYSTEM, or UAC disabled). Check TOKEN_ELEVATION to
					// determine if the token is actually elevated. Return _IsRoot
					// since there is no limited token to de-elevate to.
					TOKEN_ELEVATION Elevation = {};
					if (::GetTokenInformation(Token, TokenElevation, &Elevation, sizeof(Elevation), &ReturnLength) && Elevation.TokenIsElevated)
						return NMib::NProcess::EProcessElevation_IsRoot;

					return NMib::NProcess::EProcessElevation_IsNotElevated;
				}
			case TokenElevationTypeFull: return NMib::NProcess::EProcessElevation_IsElevated;
			case TokenElevationTypeLimited: return NMib::NProcess::EProcessElevation_IsNotElevated;
			}
		}
	}

	NStr::CStr fg_FindExecutable(NStr::CStr const &_Path, bool _bAllowLocate)
	{
		NStr::CStr Path = NFile::NPlatform::fg_ConvertFromWindowsPath(fg_FindExecutableWindows(_Path, _bAllowLocate, {}));
		if (NFile::CFile::fs_FileExists(Path, NFile::EFileAttrib_File))
			return Path;

		return _Path;
	}
}

void *NMib::NProcess::NPlatform::fg_ProcessLaunch_Open(CProcessLaunchParams const &_Params)
{
	NStorage::TCSharedPointer<CConsoleRedirector> pRedir = fg_Construct();

	pRedir->m_RefCount.f_Increase(DIfRefCountDebugging(pRedir->m_DebugSelfRef));

	auto CleanupRef = fg_OnScopeExit
		(
			[&]()
			{
				pRedir->m_RefCount.f_Decrease(DIfRefCountDebugging(pRedir->m_DebugSelfRef));
			}
		)
	;

	pRedir->f_Open(_Params);

	CleanupRef.f_Clear();

	return pRedir.f_Get();
}

void NMib::NProcess::NPlatform::fg_ProcessLaunch_Start(void *_pLaunch, EProcessLaunchCloseFlag _Flags)
{
	CConsoleRedirector *pLaunch = fg_AutoStaticCast(_pLaunch);
	pLaunch->f_StartRedirector(_Flags);
}

void NMib::NProcess::NPlatform::fg_ProcessLaunch_Close(void *_pLaunch, EProcessLaunchCloseFlag _Flags)
{
	CConsoleRedirector *pLaunch = fg_AutoStaticCast(_pLaunch);
	pLaunch->f_Close(_Flags);
	if (pLaunch->m_RefCount.f_Decrease(DIfRefCountDebugging(pLaunch->m_DebugSelfRef)) == 0)
		fg_DeleteObject(NMemory::CDefaultAllocator(), pLaunch);
}

bool NMib::NProcess::NPlatform::fg_ProcessLaunch_IsRunning(void *_pLaunch)
{
	CConsoleRedirector *pLaunch = fg_AutoStaticCast(_pLaunch);
	return pLaunch->f_IsRunning();
}

void NMib::NProcess::NPlatform::fg_ProcessLaunch_SendStdIn(void *_pLaunch, NMib::NStr::CStrIO const &_Data)
{
	CConsoleRedirector *pLaunch = fg_AutoStaticCast(_pLaunch);
	pLaunch->f_SendText(_Data);
}

void NMib::NProcess::NPlatform::fg_ProcessLaunch_CloseStdIn(void *_pLaunch)
{
	CConsoleRedirector *pLaunch = fg_AutoStaticCast(_pLaunch);
	pLaunch->f_CloseStdIn();
}

void NMib::NProcess::NPlatform::fg_ProcessLaunch_SendStdInBinary(void *_pLaunch, NContainer::CIOByteVector const &_Data)
{
	CConsoleRedirector *pLaunch = fg_AutoStaticCast(_pLaunch);
	pLaunch->f_SendBinary(_Data);
}

fp64 NMib::NProcess::NPlatform::fg_ProcessLaunch_GetRunningTime(void *_pLaunch)
{
	CConsoleRedirector *pLaunch = fg_AutoStaticCast(_pLaunch);
	return pLaunch->f_GetRunningTime();
}

umint NMib::NProcess::NPlatform::fg_ProcessLaunch_GetID(void *_pLaunch)
{
	CConsoleRedirector *pLaunch = fg_AutoStaticCast(_pLaunch);
	return pLaunch->f_GetID();
}

void NMib::NProcess::NPlatform::fg_ProcessLaunch_Stop(void *_pLaunch)
{
	CConsoleRedirector *pLaunch = fg_AutoStaticCast(_pLaunch);

	pLaunch->f_StopProcess();
}

void NMib::NProcess::NPlatform::fg_ProcessLaunch_StopGroup(void *_pLaunch)
{
	CConsoleRedirector *pLaunch = fg_AutoStaticCast(_pLaunch);

	pLaunch->f_StopProcess();
}

void NMib::NProcess::NPlatform::fg_ProcessLaunch_TerminateTree(void *_pLaunch)
{
	CConsoleRedirector *pLaunch = fg_AutoStaticCast(_pLaunch);
	NStr::CStr Log;
	pLaunch->f_TerminateTree(Log);
}

NMib::NProcess::CProcessStatistics NMib::NProcess::NPlatform::fg_ProcessLaunch_GetExecutionStatistics(void *_pLaunch)
{
	CConsoleRedirector *pLaunch = fg_AutoStaticCast(_pLaunch);
	CProcessStatistics Return;
	if (pLaunch->f_GetChildProcess())
	{
		FILETIME CreateTime1;
		FILETIME ExitTime1;
		FILETIME KernelTime1;
		FILETIME UserTime1;
		if (GetProcessTimes(pLaunch->f_GetChildProcess(), &CreateTime1, &ExitTime1, &KernelTime1, &UserTime1))
		{
			NTime::CTime CreateTime = NFile::NPlatform::fg_Win32_FileTimeToMalterlibTime(CreateTime1);
			NTime::CTimeSpan KernelTime = NFile::NPlatform::fg_Win32_FileTimeToMalterlibTimeSpan(KernelTime1);
			NTime::CTimeSpan UserTime = NFile::NPlatform::fg_Win32_FileTimeToMalterlibTimeSpan(UserTime1);
			NTime::CTimeSpan RunTime = NTime::CTime::fs_NowUTC() - CreateTime;

			fp64 KernelSeconds = KernelTime.f_GetSecondsFraction();
			fp64 UserSeconds = UserTime.f_GetSecondsFraction();
			fp64 RunSeconds = RunTime.f_GetSecondsFraction();

			Return.m_Statistics("CPU utilization Total", CProcessStat(EProcessStatUnit_Fraction, (KernelSeconds + UserSeconds) / RunSeconds));
			Return.m_Statistics("CPU utilization User", CProcessStat(EProcessStatUnit_Fraction, UserSeconds / RunSeconds));
			Return.m_Statistics("CPU utilization Kernel", CProcessStat(EProcessStatUnit_Fraction, KernelSeconds / RunSeconds));
		}
	}
	return Return;
}

NMib::NProcess::CProcessStatistics NMib::NProcess::NPlatform::fg_ProcessLaunch_GetMemoryStatistics(void *_pLaunch)
{
	CConsoleRedirector *pLaunch = fg_AutoStaticCast(_pLaunch);
	CProcessStatistics Return;
	if (pLaunch->f_GetChildProcess())
	{
		PROCESS_MEMORY_COUNTERS_EX MemoryInfo;
		NMemory::fg_MemClear(MemoryInfo);

		if (GetProcessMemoryInfo(pLaunch->f_GetChildProcess(), (PROCESS_MEMORY_COUNTERS *)&MemoryInfo, sizeof(MemoryInfo)))
		{
			Return.m_Statistics("Working set size", CProcessStat(EProcessStatUnit_Bytes, MemoryInfo.WorkingSetSize, 1024 * 1024));
			Return.m_Statistics("Paged pool usage", CProcessStat(EProcessStatUnit_Bytes, MemoryInfo.QuotaPagedPoolUsage, 1024));
			Return.m_Statistics("Non paged pool usage", CProcessStat(EProcessStatUnit_Bytes, MemoryInfo.QuotaNonPagedPoolUsage, 1024));
			Return.m_Statistics("Page file usage", CProcessStat(EProcessStatUnit_Bytes, MemoryInfo.PagefileUsage, 1024 * 1024));
			Return.m_Statistics("Private usage", CProcessStat(EProcessStatUnit_Bytes, MemoryInfo.PrivateUsage, 1024 * 1024));
			Return.m_Statistics("Page faults", CProcessStat(EProcessStatUnit_GeneralNumber, MemoryInfo.PageFaultCount, 1, "faults"));
		}
	}
	return Return;
}

NMib::NProcess::CProcessStatistics NMib::NProcess::NPlatform::fg_ProcessLaunch_GetOverallExecutionStatistics(void *_pLaunch)
{
	CConsoleRedirector *pLaunch = fg_AutoStaticCast(_pLaunch);
	return pLaunch->f_GetOverallExecutionStatistics();
}

NMib::NProcess::CProcessStatistics NMib::NProcess::NPlatform::fg_ProcessLaunch_GetOverallMemoryStatistics(void *_pLaunch)
{
	CConsoleRedirector *pLaunch = fg_AutoStaticCast(_pLaunch);
	return pLaunch->f_GetOverallMemoryStatistics();
}

void NMib::NProcess::NPlatform::fg_ProcessLaunch_CancelAll()
{
	auto &SubSystem = *g_SubSystem_Process_Platform_Windows_Launch;
	bool bDoneCancel = false;
	while (1)
	{
		{
			DMibLock(SubSystem.m_LaunchesLock);
			if (SubSystem.m_Launches.f_IsEmpty())
				return;
			if (!bDoneCancel)
			{
				bDoneCancel = true;
				for (auto Iter = SubSystem.m_Launches.f_GetIterator(); Iter; ++Iter)
					static_cast<CConsoleRedirector *>(Iter.f_GetCurrent())->f_Cancel();
			}
		}
		Sleep(1);
	}
}

void NMib::NProcess::NPlatform::fg_Process_RegisterURLHandler(NMib::NStr::CStr const &_Protocol, NMib::NStr::CStr const &_ExePath, NMib::NStr::CStr const &_Params)
{
	NMib::NPlatform::CWin32_Registry Registry(NMib::NPlatform::CWin32_Registry::ERegRoot_CurrentUser, NStr::CStr(NStr::CStr::CFormat("Software\\Classes\\{}") << _Protocol));

	NStr::CStr ProgramPath = _ExePath.f_ReplaceChar('/', '\\');

	Registry.f_Write("", "", (NStr::CStr::CFormat("URL: {} Protocol") << _Protocol).f_GetStr());
	Registry.f_Write("", "URL Protocol", "");

	Registry.f_Write("DefaultIcon", "", ProgramPath);
	Registry.f_Write("shell\\open\\command", "", "\"" + ProgramPath + "\" " + _Params);
}

void NMib::NProcess::NPlatform::fg_Process_DeRegisterURLHandler(NMib::NStr::CStr const &_Protocol)
{
	NMib::NPlatform::CWin32_Registry Registry(NMib::NPlatform::CWin32_Registry::ERegRoot_CurrentUser, NStr::CStr(NStr::CStr::CFormat("Software\\Classes\\{}") << _Protocol));

	if (Registry.f_KeyExists(""))
		Registry.f_DeleteKey("");
}

void NMib::NProcess::NPlatform::fg_Process_RegisterAtStartup(NMib::NStr::CStr const &_ExePath, NMib::NStr::CStr const &_Params, NMib::NStr::CStr const &_Name)
{
	NMib::NPlatform::CWin32_Registry Registry(NMib::NPlatform::CWin32_Registry::ERegRoot_CurrentUser, "Software\\Microsoft\\Windows\\CurrentVersion\\Run");

	NContainer::TCVector<NStr::CStr> Values;
	Registry.f_EnumValues("", Values);

	NStr::CStr Name = _Name;
	if (Name.f_IsEmpty())
		Name = fg_GetSys()->f_GetProgramName();

	NStr::CStr NamePrefix = Name + "_";
	NStr::CStr CommandLine = _ExePath.f_ReplaceChar('/', '\\') + " " + _Params;

	for (auto VIter = Values.f_GetIterator(); VIter; ++VIter)
	{
		if (VIter->f_CmpNoCase(Name) == 0 || VIter->f_StartsWith(NamePrefix))
		{
			if (Registry.f_Read_Str("", *VIter).f_CmpNoCase(CommandLine) == 0)
				return;
		}
	}

	int iCount = 0;
	NStr::CStr ValueName = Name;
	while (1)
	{
		if (!Registry.f_ValueExists("", ValueName))
		{
			Registry.f_Write("", ValueName, CommandLine);

			break;
		}

		ValueName = NStr::CStr(NMib::NStr::CStr::CFormat("{}_{}") << Name << iCount);
		++iCount;
	}
}

void NMib::NProcess::NPlatform::fg_Process_DeRegisterAtStartup(NMib::NStr::CStr const &_ExePath, NMib::NStr::CStr const &_Params, NMib::NStr::CStr const &_Name)
{
	NMib::NPlatform::CWin32_Registry Registry(NMib::NPlatform::CWin32_Registry::ERegRoot_CurrentUser, "Software\\Microsoft\\Windows\\CurrentVersion\\Run");

	NContainer::TCVector<NStr::CStr> Values;
	Registry.f_EnumValues("", Values);

	NStr::CStr Name = _Name;
	if (Name.f_IsEmpty())
		Name = fg_GetSys()->f_GetProgramName();
	NStr::CStr NamePrefix = Name + "_";
	NStr::CStr CommandLine = _ExePath.f_ReplaceChar('/', '\\') + " " + _Params;

	for (auto VIter = Values.f_GetIterator(); VIter; ++VIter)
	{
		if (VIter->f_CmpNoCase(Name) == 0 || VIter->f_StartsWith(NamePrefix))
		{
			if (Registry.f_Read_Str("", *VIter).f_CmpNoCase(CommandLine) == 0)
			{
				Registry.f_DeleteValue("", *VIter);
				return;
			}
		}
	}
}

NMib::NProcess::EProcessElevation NMib::NProcess::NPlatform::fg_Process_GetElevation()
{
	return fg_Process_GetElevation(::GetCurrentProcess());
}
