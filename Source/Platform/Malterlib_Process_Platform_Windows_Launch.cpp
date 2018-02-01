// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Process/ProcessLaunch>
#include <Mib/Core/PlatformSpecific/WindowsOptional>
#include <Mib/Core/PlatformSpecific/WindowsRegistry>
#include <Mib/Core/PlatformSpecific/WindowsError>
#include <Mib/Core/PlatformSpecific/WindowsFilePath>
#include <Mib/Core/PlatformSpecific/WindowsFile>
#include <Mib/Core/PlatformSpecific/WindowsInject>
#include <Mib/Core/PlatformSpecific/Windows>

#include <Mib/Process/Platform>
#include "Malterlib_Process_Platform_Windows_Launch.h"
#include "Malterlib_Process_Platform_Windows.h"

#include <Tlhelp32.h>
#include <wtsapi32.h>
#pragma comment(lib, "wtsapi32.lib")
#include <Psapi.h>

namespace NMib
{
	namespace NProcess
	{
		namespace NPlatform
		{

			class CProcessLaunchLink
			{
			public:
				DMibListLinkDS_Link(CProcessLaunchLink, m_Link);	
			};

			struct CSubSystem_Process_Platform_Windows_Launch : public CSubSystem
			{
				NThread::CMutual m_LaunchesLock;
				DMibListLinkDS_List(CProcessLaunchLink, m_Link) m_Launches;
				
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
			
			TCSubSystem<CSubSystem_Process_Platform_Windows_Launch, ESubSystemDestruction_BeforeMemoryManager> g_SubSystem_Process_Platform_Windows_Launch = {DAggregateInit};

			CProcessEntry::~CProcessEntry()
			{
				m_AllProcess.f_DeleteAllDefiniteType();
			}
			bint CProcessEntry::operator == (uint32 _Process) const
			{
				return m_Process == _Process;
			}

			void CProcessEntry::f_MapProcess(uint32 _ID, uint32 _ParentID, NStr::CWStr _FileName, const NTime::CTime &_CreationTime)
			{
				CProcessEntry * pEntry = m_AllProcess.f_Find(_ID);

				if (!pEntry)
				{
					pEntry = DMibNew CProcessEntry;
					pEntry->m_Process = _ID;
					m_AllProcess.f_Insert(pEntry);

				}
				pEntry->m_FileName = _FileName;
				pEntry->m_CreationTime = _CreationTime;

				CProcessEntry *pParent = m_AllProcess.f_Find(_ParentID);

				if (!pParent)
				{
					pParent = DMibNew CProcessEntry;
					pParent->m_Process = _ParentID;
					m_AllProcess.f_Insert(pParent);
				}

				pParent->m_Children.f_Insert(pEntry);
			}

			void CProcessEntry::f_KillTree(NStr::CStr &_Log, aint _Depth)
			{
				HANDLE hProcess = OpenProcess(PROCESS_TERMINATE, false, m_Process);
				if (hProcess)
				{
					_Log += NStr::CStr(NStr::CStr::CFormat("{sf ,sj*}Killed process id {}: {}" DMibNewLine) << "" << _Depth*3 << m_Process << m_FileName);
					TerminateProcess(hProcess, 255);
					CloseHandle(hProcess);
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

				NAtomic::TCAtomicAggregate<uint32> g_PipeSerialNumber = {0};


				uint32 fg_GetActiveSessionID()
				{
					uint32 ProcessID = GetCurrentProcessId();

					uint32 SessionID = 0xFFFFffff;
					ProcessIdToSessionId(ProcessID, &SessionID);

					if (SessionID != 0xFFFFffff)
						return SessionID;

			/*		if (NLocal::g_fWTSGetActiveConsoleSessionId)
						return NLocal::g_fWTSGetActiveConsoleSessionId();*/

					return 0;
				}

				NStr::CStr fg_GetProcessName(void *_pProcess)
				{
					NStr::CWStr NameW;
					bint bFailed = false;
					if (GetProcessImageFileName(_pProcess, NameW.f_GetStr(1024), 1024))
					{
						return NMib::NFile::CFile::fs_GetFile(NFile::NPlatform::fg_ConvertFromWindowsPath(NameW));
					}

					return NStr::CStr();
				}

				template <typename tf_CStrType>
				tf_CStrType fg_GetWinPathSepEscaped(tf_CStrType &_Str, const ch8 *_pSep)
				{
					tf_CStrType Ret;
					const typename tf_CStrType::CChar *pParse = _Str;
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
					{
						Ret = fg_RemoveEscapeWinPath(Ret);
					}
					return Ret;
				}

				NStr::CStr fg_ExpandEnvironmentVars(NStr::CStr _Path)
				{
					NStr::CStr Path = _Path.f_ReplaceChar('\\', '/');
					NStr::CStr RetPath;
					while (!Path.f_IsEmpty())
					{
						NStr::CStr SubPath = fg_GetStrSep(Path, "/");
						if (SubPath.f_IsEmpty())
						{
							RetPath += "/";
						}
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

				template <typename tf_CStrType>
				tf_CStrType fg_RemoveEscapeWinPath(tf_CStrType &_Str)
				{
					tf_CStrType Ret;
					const typename tf_CStrType::CChar *pParse = _Str;
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


				void fg_TerminateProcessTree(HANDLE _hProcess, NStr::CStr &_Log)
				{
					_Log += "\r\n\r\nProcess timed out and was killed along with any child processes:" DMibNewLine;
					{
						uint32 ProcessIDToKill = GetProcessId(_hProcess);
						CProcessEntry RootProcess;
						HANDLE hProcessSnap;
						PROCESSENTRY32 pe32;

						// Take a snapshot of all processes in the system.
						hProcessSnap = CreateToolhelp32Snapshot( TH32CS_SNAPPROCESS, 0 );
						if( hProcessSnap != INVALID_HANDLE_VALUE )
						{
							pe32.dwSize = sizeof( PROCESSENTRY32 );

							if(Process32First( hProcessSnap, &pe32 ) )
							{
								do
								{
									uint32 ParentProcess = uint32(-1);
									HANDLE pParentProcess = OpenProcess(PROCESS_QUERY_INFORMATION, false, pe32.th32ParentProcessID);
									HANDLE pThisProcess = OpenProcess(PROCESS_QUERY_INFORMATION, false, pe32.th32ProcessID);

									FILETIME CreateTime1;
									FILETIME ExitTime1;
									FILETIME KernelTime1;
									FILETIME UserTime1;

									NTime::CTime CreationTime;

									if (GetProcessTimes(pThisProcess, &CreateTime1, &ExitTime1, &KernelTime1, &UserTime1))
									{
										CreationTime = NFile::NPlatform::fg_Win32_FileTimeToMalterlibTime(CreateTime1);
									}

									FILETIME CreateTime;
									FILETIME ExitTime;
									FILETIME KernelTime;
									FILETIME UserTime;
									if (GetProcessTimes(pParentProcess, &CreateTime, &ExitTime, &KernelTime, &UserTime))
									{
										if ((uint64&)CreateTime1 >= (uint64&)CreateTime)
										{
											ParentProcess = pe32.th32ParentProcessID;
										}
									}

									if (pParentProcess)
										CloseHandle(pParentProcess);
									if (pThisProcess)
										CloseHandle(pThisProcess);

									RootProcess.f_MapProcess(pe32.th32ProcessID, ParentProcess, pe32.szExeFile, CreationTime);
								} 
								while( Process32Next( hProcessSnap, &pe32 ) );
							}

							CloseHandle( hProcessSnap );
						}

						//RootProcess.f_TraceTree(_Log);

						CProcessEntry *pToKill = RootProcess.m_AllProcess.f_Find(ProcessIDToKill);
						if (pToKill)
							pToKill->f_KillTree(_Log);
					}

				}

				class CConsoleRedirector : public NThread::CThread, public NPtr::TCSharedPointerIntrusiveBase<>, CProcessLaunchLink
				{
					friend class CMultiProgramStarter;
				public:
					CConsoleRedirector();
					virtual ~CConsoleRedirector();

					NStr::CStr f_GetThreadName();
					aint f_Main();

					DMibRefcountDebuggingOnly(NPtr::CRefCountDebugReference m_DebugSelfRef);
					DMibRefcountDebuggingOnly(NPtr::CRefCountDebugReference m_DebugSelfThreadRef);

				private:
					NThread::CEventAutoResetReportable mp_Event;

					NTime::CClock m_TimeSinceStart;

					NThread::CMutual m_ExitTimeLock;
					fp64 m_ExitTime;

					static void WINAPI fs_StdOutReadFinished(DWORD dwErrorCode, DWORD dwNumberOfBytesTransfered, LPOVERLAPPED lpOverlapped);
					static void WINAPI fs_StdErrReadFinished(DWORD dwErrorCode, DWORD dwNumberOfBytesTransfered, LPOVERLAPPED lpOverlapped);

				protected:
					HANDLE mp_hStdinWrite;	// write end of child's stdin pipe
					HANDLE mp_hStdoutRead;	// read end of child's stdout pipe
					HANDLE mp_hStderrRead;	// read end of child's stderr pipe
					HANDLE mp_hChildProcess;

					HANDLE mp_hSandboxJob;

					OVERLAPPED mp_StdOutRead;
					NContainer::TCVector<uint8> mp_StdOutReadBuffer;
					OVERLAPPED mp_StdErrRead;
					NContainer::TCVector<uint8> mp_StdErrReadBuffer;

					NMib::NProcess::CProcessLaunchParams mp_LastLaunchOptions;

					uint32 mp_ReturnValue;
					uint32 mp_ProcessID;
					NThread::CMutual mp_NeedTerminationLock;
					NMib::NProcess::EProcessLaunchCloseFlag mp_NeedTermination;
					bint mp_bNeedWait;
					bint mp_bStarted;

					bint fp_LaunchChild(HANDLE _hStdOut, HANDLE _hStdIn, HANDLE _hStdErr, NStr::CStr &_Errors);
					int fp_RedirectStdout();
					int fp_RedirectStderr();
					void fp_DestroyHandle(HANDLE &_Handle);
					void fp_ClearSandbox();
					void fp_TerminateSandbox(NStr::CStr &_Output);
					void fp_Close();
					bint fp_DoStart(NStr::CStr &_Errors);

				protected:
					void fp_OnLaunched(NMib::NStr::CStr const &_Error, void *_pProcess, bool _bSuccess);
					void fp_OnOutput(NMib::NProcess::EProcessLaunchOutputType _OutputType, NMib::NStr::CStr const &_Output);
					void fp_OnExit(uint32 _ExitCode);
					virtual bool f_DestroyThread() override;

				public:
					bint f_Open(NMib::NProcess::CProcessLaunchParams const &_Options);
					bint f_Start(EProcessLaunchCloseFlag _Flags);
					void f_Close(NMib::NProcess::EProcessLaunchCloseFlag _Flags);
					void f_Cancel();
					fp64 f_GetRunningTime();
					uint32 f_GetExitCode();
					bint f_IsRunning();
					bint f_SendText(NStr::CStrSecure const &_Data);
					void f_CloseStdIn();
					void f_SendBinary(NContainer::TCVector<uint8, NMem::CAllocator_HeapSecure> const &_Data);
					mint f_GetID() const;
					HANDLE f_GetChildProcess() const
					{
						return mp_hChildProcess;
					}
				};

				#if 0
				#define DEnableSandboxLock
				#endif


				/***************************************************************************************************\
				|¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯|
				| Console redirector																				|
				|___________________________________________________________________________________________________|
				\***************************************************************************************************/


				CConsoleRedirector::CConsoleRedirector()
					: mp_hStdinWrite(nullptr)
					, mp_bStarted(false)
					, mp_NeedTermination(NMib::NProcess::EProcessLaunchCloseFlag_None)
					, mp_bNeedWait(false)
					, mp_hStdoutRead(nullptr)
					, mp_hStderrRead(nullptr)
					, mp_hChildProcess(nullptr)
					, mp_hSandboxJob(nullptr)
					, mp_ProcessID(0)
					, m_ExitTime(-1.0)
				{
					m_TimeSinceStart.f_Start();
					mp_ReturnValue = ~uint32(0);

					auto &SubSystem = *g_SubSystem_Process_Platform_Windows_Launch;
					{
						DMibLock(SubSystem.m_LaunchesLock);
						SubSystem.m_Launches.f_Insert(this);
					}
				}

				void CConsoleRedirector::fp_TerminateSandbox(NStr::CStr &_Output)
				{
					if (mp_hSandboxJob)
					{
						if (!TerminateJobObject(mp_hSandboxJob, 255))
						{
							_Output += NStr::CStr::CFormat("TerminateJobObject failed with: {}" DMibNewLine) << NMib::NPlatform::fg_Win32_GetLastErrorStr(GetLastError());
						}
						CloseHandle(mp_hSandboxJob);
						mp_hSandboxJob = nullptr;
					}
				}

				void CConsoleRedirector::fp_ClearSandbox()
				{
					NStr::CStr Errors;
					fp_TerminateSandbox(Errors);
				}

				CConsoleRedirector::~CConsoleRedirector()
				{
					fp_Close();
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

				NStr::CWStr fg_FindExecutable(NStr::CStr const &_Path, bint _bAllowLocate)
				{
					// First look in current dir
					NStr::CWStr FullPathW = NFile::NPlatform::fg_ConvertToWindowsPath(_Path, true);
					NStr::CStr FullPath = NFile::NPlatform::fg_ConvertFromWindowsPath(FullPathW);
					if (NFile::CFile::fs_FileExists(FullPath, NFile::EFileAttrib_File))
						return FullPathW;

					if (!_bAllowLocate)
						return NFile::NPlatform::fg_ConvertToWindowsPath(_Path, false);
					NStr::CWStr WindowsPath = NFile::NPlatform::fg_ConvertToWindowsPath(_Path, false);

					NStr::CWStr String;
					if (FindExecutableW(WindowsPath, nullptr, String.f_GetStr(MAX_PATH+1)))
						return String;
					else
						return NFile::NPlatform::fg_ConvertToWindowsPath(_Path, false);
				}

				bool fg_SetLimitsOnJob(NMib::NProcess::CProcessLaunchParams const &_LaunchParams, HANDLE _hJob, NStr::CStr &_Errors)
				{
					bool bFailedLaunch = false;
					JOBOBJECT_BASIC_LIMIT_INFORMATION LimitInfo;
					NMem::fg_MemClear(LimitInfo);

					DWORD ReturnLength;
					QueryInformationJobObject(_hJob, JobObjectBasicLimitInformation, &LimitInfo, sizeof(LimitInfo), &ReturnLength);

					bool bSetLimits = false;

					if (_LaunchParams.m_CPUUsage > 0.0f)
					{
						/*
						JOBOBJECT_CPU_RATE_CONTROL_INFORMATION RateInfo;

						// This does not really work...

						fg_MemClear(RateInfo);

						DWORD ReturnLength;
						bool bInfo = QueryInformationJobObject(_hJob, JobObjectCpuRateControlInformation, &RateInfo, sizeof(RateInfo), &ReturnLength);

						RateInfo.ControlFlags |= JOB_OBJECT_CPU_RATE_CONTROL_ENABLE | JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP;
						RateInfo.CpuRate = fg_Clamp((_LaunchParams.m_CPUUsage * fp32(100.0f)).f_ToIntRound(), 1, 100*100);
						//RateInfo.CpuRate = 50;

						BOOL bOK = SetInformationJobObject(_hJob, JobObjectCpuRateControlInformation, &RateInfo, sizeof(RateInfo));

						if (!bOK)*/
						{
			//				HRESULT Error = GetLastError();
				//			DMibDTrace("Error: {}" DMibNewLine, NMib::NPlatform::fg_Win32_GetLastErrorStr(Error));

							DWORD_PTR ProcessAffinity, SystemAffinity;

							BOOL bOK = GetProcessAffinityMask(GetCurrentProcess(), &ProcessAffinity, &SystemAffinity);

							if (bOK)
							{
								DWORD_PTR Affinity = 0;

								{ // Use the last X processors.
									mint nProcessors = fg_NumBitsSet(ProcessAffinity);

									mint nAllowedProcessors = (fp32(nProcessors) * (fp32(_LaunchParams.m_CPUUsage))).f_ToInt();
									nAllowedProcessors = fg_Max(mint(1), nAllowedProcessors);

									DWORD_PTR Bit = DWORD_PTR(1) << (DWORD_PTR)( (sizeof(DWORD_PTR) * 8) - 1);

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
						BOOL bOK = SetInformationJobObject(_hJob, JobObjectBasicLimitInformation, &LimitInfo, sizeof(LimitInfo));
						if (!bOK)
						{
							_Errors += NStr::CStr::CFormat("Failed to set job affinity mask: {}" DMibNewLine) << NMib::NPlatform::fg_Win32_GetLastErrorStr(GetLastError());
							bFailedLaunch = true;
						}
					}
					return bFailedLaunch;
				}

				bint CConsoleRedirector::fp_LaunchChild(HANDLE _hStdOut, HANDLE _hStdIn, HANDLE _hStdErr, NStr::CStr &_Errors)
				{
					void *pOldvalue = nullptr;

					if (NLocal::g_fWow64DisableWow64FsRedirection)
						NLocal::g_fWow64DisableWow64FsRedirection(&pOldvalue);

					auto Cleanup
						= fg_OnScopeExit
						(
							[&]
							{
								if (NLocal::g_fWow64RevertWow64FsRedirection)
									NLocal::g_fWow64RevertWow64FsRedirection(pOldvalue);
							}
						)
					;
		
					bool bCreateJob = false; // Should a windows job be created to manage the new process?

					NStr::CStr Program = mp_LastLaunchOptions.m_Target;
					NStr::CStr Extension = NFile::CFile::fs_GetExtension(Program);
					if (		
								mp_LastLaunchOptions.m_LaunchType == EProcessLaunchType_Executable
							&&	(Extension.f_CmpNoCase("com") == 0
							||	Extension.f_IsEmpty()))
					{
						NStr::CStr Path = NFile::NPlatform::fg_ConvertFromWindowsPath(fg_FindExecutable(Program, mp_LastLaunchOptions.m_bAllowExecutableLocate));
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
						bint bTryCreateProcess = false;
						aint MaxLen = _MAX_PATH;
						NStr::CWStr FileW = NFile::NPlatform::fg_ConvertToWindowsPath(mp_LastLaunchOptions.m_Target, false, MaxLen);
						if ((mp_LastLaunchOptions.m_Operation == "open" || mp_LastLaunchOptions.m_Operation == "") && NFile::CFile::fs_GetExtension(mp_LastLaunchOptions.m_Target).f_CmpNoCase("exe") == 0)
						{
							Program = mp_LastLaunchOptions.m_Target;
							Params = mp_LastLaunchOptions.m_Parameters;
							bTryCreateProcess = true;
						}

						bint bIsURL = false;

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
												Params = LocalPath.f_Replace("%1", NStr::NPlatform::fg_StrToWindows(mp_LastLaunchOptions.m_Target));
												if (Params.f_FindChar('%') < 0)
													bTryCreateProcess = true;
											}
										}
									}
								}
							}
						}
						catch (const NException::CException &)
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
											Params = LocalPath.f_Replace("%1", NFile::NPlatform::fg_ConvertToWindowsPath(mp_LastLaunchOptions.m_Target, false));
											if (Params.f_FindChar('%') < 0)
												bTryCreateProcess = true;
										}
									}
								}
							}
							catch (const NException::CException &)
							{
							}
						}

						if (bTryCreateProcess && (FileW.f_GetLen() > MaxLen || ParamsW.f_GetLen() >= 2048))
						{
							PROCESS_INFORMATION ProcessInfo;
							STARTUPINFOW StartupInfo;
							NMem::fg_MemClear(ProcessInfo);
							NMem::fg_MemClear(StartupInfo);

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
								if (::CreateProcessW(
									NFile::NPlatform::fg_ConvertToWindowsPath(Program, true),
									ParamsW.f_GetStrUniqueWritable(),
									nullptr, nullptr,
									false,
									CREATE_UNICODE_ENVIRONMENT,
									nullptr, 
									(!DirectoryW.f_IsEmpty() ? DirectoryW.f_GetStr() : nullptr) ,
									&StartupInfo,
									&ProcessInfo))
								{

									mp_hChildProcess = ProcessInfo.hProcess;
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

							NMem::fg_MemClear(ExecInfo);
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
									mp_hChildProcess = ExecInfo.hProcess;
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

						SHELLEXECUTEINFOW ExecInfo;

						NStr::CWStr File = fg_FindExecutable(Program, mp_LastLaunchOptions.m_bAllowExecutableLocate);;
						NStr::CWStr Params = NStr::NPlatform::fg_StrToWindows(mp_LastLaunchOptions.m_Parameters);
						NStr::CWStr Directory = !mp_LastLaunchOptions.m_WorkingDirectory.f_IsEmpty() ? NFile::NPlatform::fg_ConvertToWindowsPath(mp_LastLaunchOptions.m_WorkingDirectory, true) : NStr::CWStr();

						//DDTrace("FileLen: {}\n", File.f_GetLen());

						NMem::fg_MemClear(ExecInfo);
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
						else
						{
							mp_hChildProcess = ExecInfo.hProcess;
							mp_ProcessID = GetProcessId(ExecInfo.hProcess);
							return true;
						}
					}
					else
					{
						HANDLE hToken = nullptr;

						DWORD CreateProcessFlags = 0;

						if (mp_LastLaunchOptions.m_bCreateNewProcessGroup)
							CreateProcessFlags |= CREATE_NEW_PROCESS_GROUP;

						auto Environment = mp_LastLaunchOptions.m_Environment;

						NStr::CWStr ProgramPathFull = fg_FindExecutable(Program, mp_LastLaunchOptions.m_bAllowExecutableLocate);

						if (ProgramPathFull.f_IsEmpty())
						{
							using namespace NStr;
							_Errors += "Failed to find an executable for '{}'\n"_f << Program;
							return false;
						}

						NStr::CStr SandboxDll;
						NStr::CStr SandboxFullPath;
						DWORD BinaryType;

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
							HANDLE hProcessSnap;
							PROCESSENTRY32 pe32;

							HANDLE hSessionToken = nullptr;
							if (WTSQueryUserToken(ActiveSessionID, &hSessionToken) == 0)
							{
								uint32 Error = GetLastError();
								_Errors += NStr::CStr::CFormat("{}" DMibNewLine) << NMib::NPlatform::fg_Win32_GetLastErrorStr(Error);
							}

							if (hSessionToken != nullptr)
							{
								if (DuplicateTokenEx(hSessionToken, MAXIMUM_ALLOWED, NULL, SecurityImpersonation, TokenPrimary, &hToken) == 0)
								{
									uint32 Error = GetLastError();
									_Errors += NStr::CStr::CFormat("{}" DMibNewLine) << NMib::NPlatform::fg_Win32_GetLastErrorStr(Error);
								}

								CloseHandle(hSessionToken);
							}

							if (hToken == nullptr)
							{
								for (int i = 0; i < 3 && hToken == nullptr; ++i)
								{
									// Take a snapshot of all processes in the system.
									hProcessSnap = CreateToolhelp32Snapshot( TH32CS_SNAPPROCESS, 0 );
									if( hProcessSnap != INVALID_HANDLE_VALUE )
									{
										pe32.dwSize = sizeof( PROCESSENTRY32 );

										if(Process32First( hProcessSnap, &pe32 ) )
										{
											HANDLE pThisProcess = nullptr;
											do
											{
												uint32 SessionID = 0xFFFFffff;
												ProcessIdToSessionId(pe32.th32ProcessID, &SessionID);

												if (SessionID != ActiveSessionID)
													continue;

												pThisProcess = OpenProcess(MAXIMUM_ALLOWED, false, pe32.th32ProcessID);
												if (!pThisProcess)
													continue;

												auto Elevation = fg_Process_GetElevation(pThisProcess);
												if (Elevation != NMib::NProcess::EProcessElevation_IsNotElevated)
													continue;
												NStr::CStr Name = fg_GetProcessName(pThisProcess);
												if (
													i == 0 && Name.f_CmpNoCase("explorer.exe") == 0 
													|| i == 1 && Name.f_CmpNoCase("LogonUI.exe") == 0 
													|| i == 2 && Name != "")
												{
													HANDLE hProcessToken;
													if (OpenProcessToken(pThisProcess, TOKEN_DUPLICATE, &hProcessToken))
													{
														hToken = nullptr;

														if (!DuplicateTokenEx(hProcessToken, MAXIMUM_ALLOWED, NULL, SecurityImpersonation, TokenPrimary, &hToken))
														{
				//											DMibDTrace("DuplicateTokenEx failed with {}" DMibNewLine, NMib::NPlatform::fg_Win32_GetLastErrorStr(GetLastError()));
														}
														CloseHandle(hProcessToken);
													}
													if (hToken)
														break;
												}
												CloseHandle(pThisProcess);
											} 
											while( Process32Next( hProcessSnap, &pe32 ) );
											if (pThisProcess != nullptr)
												CloseHandle(pThisProcess);
										}
										CloseHandle( hProcessSnap );
									}
								}
							}

							if (!hToken)
							{
								_Errors += "When de-elevating, found no process to copy security token from." DMibNewLine;
								return false;
							}
						}

						else if (mp_LastLaunchOptions.m_bSandboxed)
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
												NStr::CStr DestFile = NFile::CFile::fs_AppendPath(*iRoot, iFile->m_Path.f_Extract(Drive.f_GetLen()+1));

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
								if (	(		Major == 6 
											&&	Minor >= 2)
									||	(		Major > 6)
									)
								{
									bIsWin8 = true;
								}
							}
						}

						if (bCreateJob)
						{
							if (!bIsWin8)
								CreateProcessFlags |= CREATE_BREAKAWAY_FROM_JOB;
						}

						mp_ProcessID = 0;
						PROCESS_INFORMATION pi;
						STARTUPINFOW si;

						DMibSafeCheck(mp_hChildProcess == nullptr, "Error");

						NContainer::TCVector<ch16> NewEnvStrs;

						CSystemEnvironment NewEnvironment;
						if (mp_LastLaunchOptions.m_bMergeEnvironment)
						{
							CSystemEnvironment OriginalEnvironment = fg_GetSys()->f_Environment();
							NewEnvironment = Environment;
							NewEnvironment += OriginalEnvironment;
						}
						else if (!Environment.f_IsEmpty())
							NewEnvironment = Environment;
						else
							NewEnvironment = fg_GetSys()->f_Environment();
						
						auto Iter = NewEnvironment.f_GetIterator();

						while (Iter)
						{
							NStr::CWStr WholeStr = NStr::NPlatform::fg_StrToWindows(Iter.f_GetKey() + "=" + *Iter);
							NewEnvStrs.f_Insert(WholeStr.f_GetStr(), WholeStr.f_GetLen() + 1);
			
							++Iter;
						}
						NewEnvStrs.f_Insert(ch16(0));

						// Set up the start up info struct.
						NMem::fg_MemClear(si);
						si.cb = sizeof(STARTUPINFOW);
						si.hStdOutput = _hStdOut;
						si.hStdInput = _hStdIn;
						si.hStdError = _hStdErr;
						si.wShowWindow = mp_LastLaunchOptions.m_bShowLaunched ? SW_SHOWNORMAL : SW_HIDE;
						si.dwFlags = STARTF_USESHOWWINDOW;

						if (mp_LastLaunchOptions.m_bDisplayBusyCursor)
							si.dwFlags |= STARTF_FORCEONFEEDBACK;
						else
							si.dwFlags |= STARTF_FORCEOFFFEEDBACK;

						if (mp_LastLaunchOptions.m_bEnableStdRedirection)
							si.dwFlags |= STARTF_USESTDHANDLES;

						// Note that dwFlags must include STARTF_USESHOWWINDOW if we
						// use the wShowWindow flags. This also assumes that the
						// CreateProcess() call will use CREATE_NEW_CONSOLE.

						// Launch the child process.

						int ProcPriority = 0;

						NStr::CWStr Params = NStr::NPlatform::fg_StrToWindows(ProgramPathFull.f_EscapeStr().f_Replace("\\\\", "\\") + " " + mp_LastLaunchOptions.m_Parameters);
						if (hToken)
						{
				
							if (NLocal::g_fCreateProcessWithTokenW)
							{
								HANDLE hThreadToken = NULL;
								ImpersonateSelf(SecurityImpersonation);
								if (!OpenThreadToken(GetCurrentThread(), MAXIMUM_ALLOWED, false, &hThreadToken))
								{
									NStr::CStr Error = NMib::NPlatform::fg_Win32_GetLastErrorStr(GetLastError());
									_Errors += NStr::CStr::CFormat("OpenThreadToken failed with : {}" DMibNewLine) << Error;
									RevertToSelf();
									return false;
								}
								else
								{
									TOKEN_PRIVILEGES tkp;
									tkp.PrivilegeCount = 1;
									LookupPrivilegeValueW(NULL, SE_INCREASE_QUOTA_NAME, &tkp.Privileges[0].Luid);
									tkp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
									AdjustTokenPrivileges(hThreadToken, FALSE, &tkp, 0, NULL, NULL);
									HRESULT dwLastErr = GetLastError();
									CloseHandle(hThreadToken);
									if (dwLastErr != ERROR_SUCCESS)
									{
										NStr::CStr Error = NMib::NPlatform::fg_Win32_GetLastErrorStr(GetLastError());
										_Errors += NStr::CStr::CFormat("AdjustTokenPrivileges failed with : {}" DMibNewLine) << Error;
										return false;
									}
								}

								if	
									(
										!NLocal::g_fCreateProcessWithTokenW
										(
											hToken
											, 0
											, ProgramPathFull
											, Params.f_GetStrUniqueWritable()
											, CREATE_UNICODE_ENVIRONMENT | fg_Win32_TranslateProcessPriority(mp_LastLaunchOptions.m_LaunchPriority) | CreateProcessFlags
											, !NewEnvStrs.f_IsEmpty() ? NewEnvStrs.f_GetArray() : nullptr
											, !mp_LastLaunchOptions.m_WorkingDirectory.f_IsEmpty() ? NFile::NPlatform::fg_ConvertToWindowsPath(mp_LastLaunchOptions.m_WorkingDirectory, true).f_GetStr() : nullptr
											, &si
											, &pi
										)
									)
								{
									RevertToSelf();
									CloseHandle(hToken);
									NStr::CStr Error = NMib::NPlatform::fg_Win32_GetLastErrorStr(GetLastError());
									_Errors += NStr::CStr::CFormat("CreateProcessWithTokenW({}, {}) failed with : {}" DMibNewLine) << ProgramPathFull << Params << Error;
									return false;
								}
								RevertToSelf();
							}
							else
							{
								if	
									(
										!::CreateProcessAsUserW
										(
											hToken
											, ProgramPathFull
											, Params.f_GetStrUniqueWritable()
											, nullptr
											, nullptr
											, true
											, CREATE_UNICODE_ENVIRONMENT | fg_Win32_TranslateProcessPriority(mp_LastLaunchOptions.m_LaunchPriority) | CreateProcessFlags
											, !NewEnvStrs.f_IsEmpty() ? NewEnvStrs.f_GetArray() : nullptr
											, !mp_LastLaunchOptions.m_WorkingDirectory.f_IsEmpty() ? NFile::NPlatform::fg_ConvertToWindowsPath(mp_LastLaunchOptions.m_WorkingDirectory, true).f_GetStr() : nullptr
											, &si
											, &pi
										)
									)
								{
									CloseHandle(hToken);
									NStr::CStr Error = NMib::NPlatform::fg_Win32_GetLastErrorStr(GetLastError());
									_Errors += NStr::CStr::CFormat("CreateProcessW({}, {}) failed with : {}" DMibNewLine) << ProgramPathFull << Params << Error;
									return false;
								}
							}
							CloseHandle(hToken);
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
										, !mp_LastLaunchOptions.m_WorkingDirectory.f_IsEmpty() ? NFile::NPlatform::fg_ConvertToWindowsPath(mp_LastLaunchOptions.m_WorkingDirectory, true).f_GetStr() : nullptr
										, &si
										, &pi
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
								HANDLE hParent = nullptr;
								auto JobCleanupParent = fg_OnScopeExit
									(
										[&]()
										{
											if (hParent)
												CloseHandle(hParent);
										}
									)
								;
								if (bIsWin8 && !mp_LastLaunchOptions.m_ProcessGroup.f_IsEmpty())
								{
									NStr::CWStr JobName = mp_LastLaunchOptions.m_ProcessGroup;
									hParent = CreateJobObjectW(nullptr, JobName);
									if (hParent)
									{
										bool bFailed = false;
										if (GetLastError() != ERROR_ALREADY_EXISTS)
										{
											NStr::CStr Errors;
											if (fg_SetLimitsOnJob(mp_LastLaunchOptions, hParent, Errors))
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
									<< NMisc::fg_GetRandomUnsigned()
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
									if (hParent)
									{
										if (!AssignProcessToJobObject(hParent, pi.hProcess))
										{
											HRESULT Error = GetLastError();
											DMibDTrace("AssignProcessToJobObject: {}" DMibNewLine, NMib::NPlatform::fg_Win32_GetLastErrorStr(Error));
										}
									}
									if (AssignProcessToJobObject(Job, pi.hProcess))
									{
										if (!bSetOnParent)
										{
											if (fg_SetLimitsOnJob(mp_LastLaunchOptions, Job, _Errors))
												bFailedLaunch = true;
										}

										if (!bFailedLaunch && mp_LastLaunchOptions.m_bSandboxed)
										{
											JOBOBJECT_EXTENDED_LIMIT_INFORMATION LimitInfo;
											NMem::fg_MemClear(LimitInfo);

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
											mp_hSandboxJob = Job;
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
							static const DWORD DefaultMemoryPriority = 5;
							static const DWORD LowMemoryPriority = 1;
							static const DWORD DefaultIoPriority = 2;
							static const DWORD LowIoPriority = 1;

							typedef struct _MEMORY_PRIORITY_INFORMATION 
							{
								ULONG MemoryPriority;
							} MEMORY_PRIORITY_INFORMATION, *PMEMORY_PRIORITY_INFORMATION;

							MEMORY_PRIORITY_INFORMATION MemoryPrio;
							MemoryPrio.MemoryPriority = LowMemoryPriority;

							auto Size = sizeof(MemoryPrio);
							if (!NLocal::g_fSetProcessInformation || !NLocal::g_fSetProcessInformation(pi.hProcess, ProcessMemoryPriority, &MemoryPrio, sizeof(MemoryPrio)))
							{
								DMibTrace("Failed to set memory priority {}\n", NMib::NPlatform::fg_Win32_GetLastErrorStr(GetLastError()));
								if (NLocal::g_fNtSetInformationProcess)
								{
									auto Result = NLocal::g_fNtSetInformationProcess(pi.hProcess, (PROCESSINFOCLASS)ProcessPagePriority, &MemoryPrio, sizeof(MemoryPrio));
									if (!NT_SUCCESS(Result))
										DMibTrace("Failed to set memory priority {}\n", NMib::NPlatform::fg_Win32_GetLastErrorStr(GetLastError()));
								}
							}

							if (NLocal::g_fNtSetInformationProcess)
							{
								ULONG IoPrio = 0;
								auto Result = NLocal::g_fNtSetInformationProcess(pi.hProcess, (PROCESSINFOCLASS)ProcessIoPriority, &IoPrio, sizeof(IoPrio));
								if (!NT_SUCCESS(Result))
									DMibTrace("Failed to set IO priority {}\n", NMib::NPlatform::fg_Win32_GetLastErrorStr(GetLastError()));
							}
						}
			#endif

						if (mp_LastLaunchOptions.m_bSandboxed && !bFailedLaunch && !mp_LastLaunchOptions.m_SandboxRoots.f_IsEmpty())
						{
							NStr::CStr Errors;
			#ifdef DArchitecture_x64
			DWORD NeededType = SCS_64BIT_BINARY;
			#else
			DWORD NeededType = SCS_32BIT_BINARY;
			#endif
							if (BinaryType == NeededType)
							{
								NStr::CWStr WindowsDllName = NFile::NPlatform::fg_ConvertToWindowsPath(SandboxFullPath, false);
								if (!NMib::NPlatform::fg_InjectDLL(pi.hProcess, pi.hThread, WindowsDllName, Errors))
								{
									_Errors += Errors;
									bFailedLaunch = true;
								}
							}
							else
							{
								// Launch process to do the injection

								NStr::CStr SandboxInjecter = NFile::CFile::fs_GetPath(SandboxFullPath) + "/" + NFile::CFile::fs_GetFileNoExt(SandboxFullPath) + ".exe";

								CProcessLaunchParams LaunchParams = CProcessLaunchParams::fs_LaunchExecutable
									(
										SandboxInjecter
										, NStr::CStr::CFormat("0x{nfh} {}") << pi.dwProcessId << pi.dwThreadId
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
		
								LaunchParams.m_fOnOutput 
									= [&](EProcessLaunchOutputType _OutputType, NStr::CStr const &_Output)
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
										}
									}
								;
								LaunchParams.m_bSandboxed = false;
								LaunchParams.m_bEnableStdRedirection = true;
								{
									CProcessLaunch Launch(LaunchParams, EProcessLaunchCloseFlag_BlockOnExit);
								}

							}
						}

						if (!bFailedLaunch && (CreateProcessFlags & CREATE_SUSPENDED))
							ResumeThread(pi.hThread);

						if (!bFailedLaunch)
						{
							mp_hChildProcess = pi.hProcess;
							mp_ProcessID = pi.dwProcessId;
						}
						else
						{
							TerminateProcess(pi.hProcess, 255);
							::CloseHandle(pi.hProcess);
							::CloseHandle(pi.hThread);
							return false;
						}
						// Close any unuseful handles
						::CloseHandle(pi.hThread);
						return true;
					}
					return false;
				}

				void WINAPI CConsoleRedirector::fs_StdOutReadFinished(DWORD dwErrorCode, DWORD dwNumberOfBytesTransfered, LPOVERLAPPED lpOverlapped)
				{
					CConsoleRedirector *pThis = (CConsoleRedirector *)lpOverlapped->hEvent;
					if (dwNumberOfBytesTransfered)
					{
						NStr::CStr Temp((ch8 const *)lpOverlapped->Pointer, dwNumberOfBytesTransfered); // Assume UTF8
						pThis->fp_OnOutput(NMib::NProcess::EProcessLaunchOutputType_StdOut, NStr::CStr(Temp));
					}

					if 
						(
							dwErrorCode == ERROR_OPERATION_ABORTED	// CancelIo was called
							|| dwErrorCode == ERROR_BROKEN_PIPE		// Pipe has been ended
							|| dwErrorCode == ERROR_NO_DATA			// Pipe closing in progress
						)
					{
						return;
					}

					if (dwErrorCode)
					{
						NStr::CStr Error = NMib::NPlatform::fg_Win32_GetLastErrorStr(dwErrorCode);
						pThis->fp_OnOutput(NMib::NProcess::EProcessLaunchOutputType_GeneralError, NStr::CStr::CFormat("Read stdout pipe error: {}" DMibNewLine) << Error);
					}

					if (::ReadFileEx(pThis->mp_hStdoutRead, pThis->mp_StdOutReadBuffer.f_GetArray(), 4096, &pThis->mp_StdOutRead, &CConsoleRedirector::fs_StdOutReadFinished))
						return;

					dwErrorCode = GetLastError();
					if 
						(
							dwErrorCode == ERROR_OPERATION_ABORTED	// CancelIo was called
							|| dwErrorCode == ERROR_BROKEN_PIPE		// Pipe has been ended
							|| dwErrorCode == ERROR_NO_DATA			// Pipe closing in progress
						)
					{
						return;
					}

					NStr::CStr Error = NMib::NPlatform::fg_Win32_GetLastErrorStr(dwErrorCode);
					pThis->fp_OnOutput(NMib::NProcess::EProcessLaunchOutputType_GeneralError, NStr::CStr::CFormat("Read stdout pipe error: {}" DMibNewLine) << Error);
				}

				void WINAPI CConsoleRedirector::fs_StdErrReadFinished(DWORD dwErrorCode, DWORD dwNumberOfBytesTransfered, LPOVERLAPPED lpOverlapped)
				{
					CConsoleRedirector *pThis = (CConsoleRedirector *)lpOverlapped->hEvent;
					if (dwNumberOfBytesTransfered)
					{
						NStr::CStr Temp((ch8 const *)lpOverlapped->Pointer, dwNumberOfBytesTransfered);
						pThis->fp_OnOutput(NMib::NProcess::EProcessLaunchOutputType_StdErr, NStr::CStr(Temp));
					}

					if 
						(
							dwErrorCode == ERROR_OPERATION_ABORTED	// CancelIo was called
							|| dwErrorCode == ERROR_BROKEN_PIPE		// Pipe has been ended
							|| dwErrorCode == ERROR_NO_DATA			// Pipe closing in progress
						)
					{
						return;
					}

					if (dwErrorCode)
					{
						NStr::CStr Error = NMib::NPlatform::fg_Win32_GetLastErrorStr(dwErrorCode);
						pThis->fp_OnOutput(NMib::NProcess::EProcessLaunchOutputType_GeneralError, NStr::CStr::CFormat("Read stderr pipe error: {}" DMibNewLine) << Error);
					}

					if (::ReadFileEx(pThis->mp_hStderrRead, pThis->mp_StdErrReadBuffer.f_GetArray(), 4096, &pThis->mp_StdErrRead, &CConsoleRedirector::fs_StdErrReadFinished))
						return;

					dwErrorCode = GetLastError();
					if 
						(
							dwErrorCode == ERROR_OPERATION_ABORTED	// CancelIo was called
							|| dwErrorCode == ERROR_BROKEN_PIPE		// Pipe has been ended
							|| dwErrorCode == ERROR_NO_DATA			// Pipe closing in progress
						)
					{
						return;
					}

					NStr::CStr Error = NMib::NPlatform::fg_Win32_GetLastErrorStr(dwErrorCode);
					pThis->fp_OnOutput(NMib::NProcess::EProcessLaunchOutputType_GeneralError, NStr::CStr::CFormat("Read stderr pipe error: {}" DMibNewLine) << Error);
				}

				int CConsoleRedirector::fp_RedirectStdout()
				{
					DMibSafeCheck(mp_hStdoutRead != nullptr, "");

					if (mp_StdOutReadBuffer.f_IsEmpty())
					{
						mp_StdOutReadBuffer.f_SetLen(4096);
						NMem::fg_MemClear(mp_StdOutRead);
						mp_StdOutRead.hEvent = (void *)this;
						mp_StdOutRead.Pointer = mp_StdOutReadBuffer.f_GetArray();
					}

					fs_StdOutReadFinished(0, 0, &mp_StdOutRead);

					return 0;
				}

				int CConsoleRedirector::fp_RedirectStderr()
				{
					DMibSafeCheck(mp_hStderrRead != nullptr, "");

					if (mp_StdErrReadBuffer.f_IsEmpty())
					{
						mp_StdErrReadBuffer.f_SetLen(4096);
						NMem::fg_MemClear(mp_StdErrRead);
						mp_StdErrRead.hEvent = (void *)this;
						mp_StdErrRead.Pointer = mp_StdErrReadBuffer.f_GetArray();
					}

					fs_StdErrReadFinished(0, 0, &mp_StdOutRead);

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
						fp_OnLaunched(Errors, mp_hChildProcess, false);
						return 0;
					}
					fp_OnLaunched(Errors, mp_hChildProcess, true);

					HANDLE WaitForHandles[4];
					WaitForHandles[0] = mp_hChildProcess;
					WaitForHandles[1] = mp_Event.m_pSemaphore;

					bint bExited = false;

					// Kickstart IO
					if (mp_hStdoutRead)
						fp_RedirectStdout();
					if (mp_hStderrRead)
						fp_RedirectStderr();

					while (true)
					{
						DWORD Object = WaitForMultipleObjectsEx(2, WaitForHandles, false, 10000, true);

						if (Object == WAIT_IO_COMPLETION)
						{
						}
						else if (Object == WAIT_OBJECT_0 || mp_hChildProcess == nullptr)
						{
							bExited = true;
							break;
						}
						else if (Object == WAIT_OBJECT_0 + 1)
						{
							NMib::NProcess::EProcessLaunchCloseFlag NeedTermination = NMib::NProcess::EProcessLaunchCloseFlag_None;
							bint bNeedWait = false;
							{
								DMibLock(mp_NeedTerminationLock);
								fg_Swap(NeedTermination, mp_NeedTermination);
								fg_Swap(bNeedWait, mp_bNeedWait);
							}

							if (NeedTermination & NMib::NProcess::EProcessLaunchCloseFlag_TerminateProcess)
							{
								NStr::CStr TempRet;
								if (!mp_LastLaunchOptions.m_bSandboxed) // If we are sandboxed just break and let the sandbox program kill all processes in sandbox
								{
									fg_TerminateProcessTree(mp_hChildProcess, TempRet);
								}
								else
								{
									fp_TerminateSandbox(TempRet);
								}
								if (!TempRet.f_IsEmpty())
									fp_OnOutput(NMib::NProcess::EProcessLaunchOutputType_TerminateMessage, TempRet);
							}
							else if (NeedTermination & NMib::NProcess::EProcessLaunchCloseFlag_StopProcess)
							{
								NStr::CStr TempRet;
								if (mp_ProcessID)
								{
									if (!GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, mp_ProcessID))
									{
										TempRet = NMib::NPlatform::fg_Win32_GetLastErrorStr();
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
								break;
						}
					}

					// Read any outstading IO
					while (SleepEx(0, true) == WAIT_IO_COMPLETION)
						;

					// Cancel IO
					CancelIo(mp_hStdoutRead);
					if (mp_hStderrRead)
						CancelIo(mp_hStderrRead);

					// Wait for cancellation
					while (SleepEx(0, true) == WAIT_IO_COMPLETION)
						;

					if (bExited)
					{
						if (mp_LastLaunchOptions.m_fOnStateChange)
						{
							DWORD ExitCode = 255;
							if (mp_hChildProcess)
							{
								GetExitCodeProcess(mp_hChildProcess, &ExitCode);

							}
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
						DMibLock(m_ExitTimeLock);
						TimeSinceStart = m_TimeSinceStart.f_GetTime();
						if (!_pProcess)
							m_ExitTime = TimeSinceStart;
					}
					if (mp_LastLaunchOptions.m_bAllowLaunchedInForground && _pProcess)
						AllowSetForegroundWindow(GetProcessId(_pProcess));
					if (mp_LastLaunchOptions.m_fOnStateChange)
					{
						if (mp_LastLaunchOptions.m_fDispatcher)
						{
							NPtr::TCSharedPointer<CConsoleRedirector> pThis = fg_Explicit(this);
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
							NPtr::TCSharedPointer<CConsoleRedirector> pThis = fg_Explicit(this);
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

				void CConsoleRedirector::fp_OnExit(uint32 _ExitCode)
				{
					fp64 TimeSinceStart;
					{
						DMibLock(m_ExitTimeLock);
						TimeSinceStart = m_TimeSinceStart.f_GetTime();
						m_ExitTime = TimeSinceStart;
					}

					if (mp_LastLaunchOptions.m_fOnStateChange)
					{
						if (mp_LastLaunchOptions.m_fDispatcher)
						{
							NPtr::TCSharedPointer<CConsoleRedirector> pThis = fg_Explicit(this);
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

				BOOL
				APIENTRY
				CreatePipeEx(
					OUT LPHANDLE lpReadPipe,
					OUT LPHANDLE lpWritePipe,
					IN LPSECURITY_ATTRIBUTES lpPipeAttributes,
					IN DWORD nSize,
					DWORD dwReadMode,
					DWORD dwWriteMode,
					NMib::NStr::CStr &_ExtendedError
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

					lpReadPipe - Returns a handle to the read side of the pipe.  Data
						may be read from the pipe by specifying this handle value in a
						subsequent call to ReadFile.

					lpWritePipe - Returns a handle to the write side of the pipe.  Data
						may be written to the pipe by specifying this handle value in a
						subsequent call to WriteFile.

					lpPipeAttributes - An optional parameter that may be used to specify
						the attributes of the new pipe.  If the parameter is not
						specified, then the pipe is created without a security
						descriptor, and the resulting handles are not inherited on
						process creation.  Otherwise, the optional security attributes
						are used on the pipe, and the inherit handles flag effects both
						pipe handles.

					nSize - Supplies the requested buffer size for the pipe.  This is
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
					DWORD dwError;

					//
					// Only one valid OpenMode flag - FILE_FLAG_OVERLAPPED
					//

					if ((dwReadMode | dwWriteMode) & (~FILE_FLAG_OVERLAPPED)) 
					{
						SetLastError(ERROR_INVALID_PARAMETER);
						return FALSE;
					}

					//
					//  Set the default timeout to 120 seconds
					//

					if (nSize == 0) {
						nSize = 4096;
						}

					NStr::CWStr PipeName 
						= NStr::CWStr::CFormat(str_utf16("\\\\.\\Pipe\\AnonOverlapped.{nfh,sf0,sj8}.{nfh,sf0,sj8}.{nfh,sf0,sj8}.{nfh,sf0,sj16}")) 
						<< GetCurrentProcessId() 
						<< ++g_PipeSerialNumber
						<< NMisc::fg_GetRandomUnsigned()
						<< &g_PipeSerialNumber
					;

					bool bRetry = true;
			Retry:
					ReadPipeHandle = CreateNamedPipeW(
										 PipeName.f_GetStr(),
										 PIPE_ACCESS_INBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE | dwReadMode,
										 PIPE_TYPE_BYTE | PIPE_WAIT,
										 1,             // Number of pipes
										 nSize,         // Out buffer size
										 nSize,         // In buffer size
										 120 * 1000,    // Timeout in ms
										 lpPipeAttributes
										 );

					if (!ReadPipeHandle || ReadPipeHandle == INVALID_HANDLE_VALUE) 
					{
						if (GetLastError() == ERROR_PIPE_BUSY)
						{
							if (bRetry)
							{
								bRetry = false;
								NTime::CClock Time;
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

					auto Cleanup
						= fg_OnScopeExit
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


					WritePipeHandle = CreateFileW(
										PipeName.f_GetStr(),
										GENERIC_WRITE,
										0,                         // No sharing
										lpPipeAttributes,
										OPEN_EXISTING,
										FILE_ATTRIBUTE_NORMAL | dwWriteMode,
										NULL                       // Template file
									  );

					if (!WritePipeHandle || INVALID_HANDLE_VALUE == WritePipeHandle) 
					{
						dwError = GetLastError();
						if (dwError == ERROR_PIPE_BUSY)
						{
							if (bRetry2)
							{
								bRetry2 = false;
								NTime::CClock Time;
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

						SetLastError(dwError);
						return FALSE;
					}

					*lpReadPipe = ReadPipeHandle;
					*lpWritePipe = WritePipeHandle;
					Cleanup.f_Clear();
					return TRUE;
				}

				bint CConsoleRedirector::fp_DoStart(NStr::CStr &_Errors)
				{
					HANDLE hStdoutReadTmp;				// parent stdout read handle
					HANDLE hStdoutWrite;	// child stdout write handle
					HANDLE hStderrReadTmp;				// parent stdout read handle
					HANDLE hStderrWrite;	// child stdout write handle
					HANDLE hStdinWriteTmp;				// parent stdin write handle
					HANDLE hStdinRead;					// child stdin read handle

					fp_Close();
					hStdoutReadTmp = nullptr;
					hStdoutWrite = nullptr;
					hStderrReadTmp = nullptr;
					hStderrWrite = nullptr;
					hStdinWriteTmp = nullptr;
					hStdinRead = nullptr;

					// Set up the security attributes struct.
					SECURITY_ATTRIBUTES sa;
					sa.nLength = sizeof(SECURITY_ATTRIBUTES);
					sa.lpSecurityDescriptor = nullptr;
					sa.bInheritHandle = TRUE;

					BOOL bOK = false;
					do
					{
						// Create a child stdout pipe.
						{
							if (mp_LastLaunchOptions.m_bEnableStdRedirection)
							{
								NStr::CStr ExtendedError;
								if (!CreatePipeEx(&hStdoutReadTmp, &hStdoutWrite, &sa, 0, FILE_FLAG_OVERLAPPED, 0, ExtendedError))
								{
									DWORD dwOsErr = ::GetLastError();
									_Errors += NStr::CStr::CFormat("CreatePipe (StdOut) failed: {} {}" DMibNewLine) << NMib::NPlatform::fg_Win32_GetLastErrorStr(dwOsErr) << ExtendedError;
									break;
								}

								if (mp_LastLaunchOptions.m_bSeparateStdErr)
								{
									if (!CreatePipeEx(&hStderrReadTmp, &hStderrWrite, &sa, 0, FILE_FLAG_OVERLAPPED, 0, ExtendedError))
									{
										DWORD dwOsErr = ::GetLastError();
										_Errors += NStr::CStr::CFormat("CreatePipe (StdErr) failed: {} {}" DMibNewLine) << NMib::NPlatform::fg_Win32_GetLastErrorStr(dwOsErr) << ExtendedError;
										break;
									}
								}

								// Create a child stdin pipe.
								if (!CreatePipeEx(&hStdinRead, &hStdinWriteTmp, &sa, 0, 0, 0, ExtendedError))
								{
									DWORD dwOsErr = ::GetLastError();
									_Errors += NStr::CStr::CFormat("CreatePipe (StdIn) failed: {} {}" DMibNewLine) << NMib::NPlatform::fg_Win32_GetLastErrorStr(dwOsErr) << ExtendedError;
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
											, hStdoutReadTmp
											, ::GetCurrentProcess()
											, &mp_hStdoutRead
											, 0
											, FALSE 			// make it uninheritable.
											, DUPLICATE_SAME_ACCESS
										)
									)
								{
									DWORD dwOsErr = ::GetLastError();
									_Errors += NStr::CStr::CFormat("DuplicateHandle failed: {}" DMibNewLine) << NMib::NPlatform::fg_Win32_GetLastErrorStr(dwOsErr);
									break;
								}

								if (mp_LastLaunchOptions.m_bSeparateStdErr)
								{
									if 
										(
											!::DuplicateHandle
											(
												::GetCurrentProcess()
												, hStderrReadTmp
												, ::GetCurrentProcess()
												, &mp_hStderrRead
												, 0
												, FALSE			// make it uninheritable.
												, DUPLICATE_SAME_ACCESS
											)
										)
									{
										DWORD dwOsErr = ::GetLastError();
										_Errors += NStr::CStr::CFormat("DuplicateHandle failed: {}" DMibNewLine) << NMib::NPlatform::fg_Win32_GetLastErrorStr(dwOsErr);
										break;
									}
								}

								if 
									(
										!::DuplicateHandle
										(
											::GetCurrentProcess()
											, hStdinWriteTmp
											, ::GetCurrentProcess()
											, &mp_hStdinWrite
											, 0
											, FALSE		// make it uninheritable.
											, DUPLICATE_SAME_ACCESS
										)
									)
								{
									DWORD dwOsErr = ::GetLastError();
									_Errors += NStr::CStr::CFormat("DuplicateHandle failed: {}" DMibNewLine) << NMib::NPlatform::fg_Win32_GetLastErrorStr(dwOsErr);
									break;
								}

								// Close inheritable copies of the handles we do not want to
								// be inherited.
								fp_DestroyHandle(hStdoutReadTmp);
								fp_DestroyHandle(hStderrReadTmp);
								fp_DestroyHandle(hStdinWriteTmp);
							}
						}

						// launch the child process
						if (!fp_LaunchChild(hStdoutWrite, hStdinRead, mp_LastLaunchOptions.m_bSeparateStdErr ? hStderrWrite : hStdoutWrite, _Errors))
							break;

						// Child is launched. Close the parents copy of those pipe
						// handles that only the child should have open.
						// Make sure that no handles to the write end of the stdout pipe
						// are maintained in this process or else the pipe will not
						// close when the child process exits and ReadFile will hang.
						fp_DestroyHandle(hStdoutWrite);
						fp_DestroyHandle(hStderrWrite);
						fp_DestroyHandle(hStdinRead);

						bOK = TRUE;
					}
					while (0);

					if (!bOK)
					{
						fp_DestroyHandle(hStdoutReadTmp);
						fp_DestroyHandle(hStdoutWrite);
						fp_DestroyHandle(hStderrReadTmp);
						fp_DestroyHandle(hStderrWrite);
						fp_DestroyHandle(hStdinWriteTmp);
						fp_DestroyHandle(hStdinRead);
						fp_Close();
					}

					return bOK;
				}

				bint CConsoleRedirector::f_Open(NMib::NProcess::CProcessLaunchParams const &_Options)
				{
					mp_LastLaunchOptions = _Options;
					return true;
				}

				bint CConsoleRedirector::f_Start(EProcessLaunchCloseFlag _Flags)
				{
					if (mp_bStarted)
						DMibError(NStr::CStrNonTracked("Launch has already been started"));
					mp_bStarted = true;
					// Increase ref count for thread
					if (mp_LastLaunchOptions.m_bThreaded)
					{
						f_RefCountIncrease(DMibRefcountDebuggingOnly(m_DebugSelfThreadRef));
						auto CleanupRef = fg_OnScopeExit
							(
								[&]()
								{
									f_RefCountDecrease(DMibRefcountDebuggingOnly(m_DebugSelfThreadRef));
								}
							)
						;

						__super::f_Start(EThreadPriority_Highest, 0, 0, true);

						CleanupRef.f_Clear();
					}
					else
					{
						{
							DMibLock(mp_NeedTerminationLock);
							mp_NeedTermination = (_Flags & (NMib::NProcess::EProcessLaunchCloseFlag_TerminateProcess | NMib::NProcess::EProcessLaunchCloseFlag_StopProcess));
							if (_Flags & (NMib::NProcess::EProcessLaunchCloseFlag_LingerUntilDone | NMib::NProcess::EProcessLaunchCloseFlag_BlockOnExit))
								mp_bNeedWait = true;
						}
						mp_Event.f_Signal();
						f_Main();
					}
					return true;
				}

				void CConsoleRedirector::fp_Close()
				{
					fp_DestroyHandle(mp_hChildProcess);
					fp_DestroyHandle(mp_hStdinWrite);
					fp_DestroyHandle(mp_hStdoutRead);
					fp_DestroyHandle(mp_hStderrRead);
					fp_DestroyHandle(mp_hSandboxJob);
				}

				bool CConsoleRedirector::f_DestroyThread()
				{
					if (f_RefCountDecrease(DMibRefcountDebuggingOnly(m_DebugSelfThreadRef)) == 0)
					{
						delete this;
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
					mp_Event.f_Signal();
				}

				fp64 CConsoleRedirector::f_GetRunningTime()
				{
					DMibLock(m_ExitTimeLock);
					if (f_IsRunning())
						return m_TimeSinceStart.f_GetTime();
					else
						return m_ExitTime;
				}

				mint CConsoleRedirector::f_GetID() const
				{
					return mp_ProcessID;
				}


				void CConsoleRedirector::f_Close(NMib::NProcess::EProcessLaunchCloseFlag _Flags)
				{

					bool bNeedWait;
					{
						{
							DMibLock(mp_NeedTerminationLock);
							mp_NeedTermination = (_Flags & (NMib::NProcess::EProcessLaunchCloseFlag_TerminateProcess | NMib::NProcess::EProcessLaunchCloseFlag_StopProcess));
							if (_Flags & (NMib::NProcess::EProcessLaunchCloseFlag_LingerUntilDone | NMib::NProcess::EProcessLaunchCloseFlag_BlockOnExit))
								mp_bNeedWait = true;
							bNeedWait = mp_bNeedWait;
						}
						mp_Event.f_Signal();
					}
					if (_Flags & NMib::NProcess::EProcessLaunchCloseFlag_BlockOnExit || !bNeedWait)
						f_Stop(true);
				}

				uint32 CConsoleRedirector::f_GetExitCode()
				{
					return mp_ReturnValue;
				}

				bint CConsoleRedirector::f_IsRunning()
				{
					return (f_GetState() == NThread::EThreadState_Running);
				}

				bint CConsoleRedirector::f_SendText(NStr::CStrSecure const &_Data)
				{
					if (!mp_hStdinWrite)
						return FALSE;

					if (::GetFileType(mp_hStdinWrite) == FILE_TYPE_CHAR)
					{
						DWORD dwWritten;
						NStr::CWStrSecure Output = _Data;
						return ::WriteConsoleW(mp_hStdinWrite, Output.f_GetStr(), Output.f_GetLen(), &dwWritten, nullptr);
					}
					else
					{
						DWORD dwWritten;
						NStr::CStrSecure Output = _Data;
						return ::WriteFile(mp_hStdinWrite, Output.f_GetStr(), Output.f_GetLen(), &dwWritten, nullptr);
					}
				}

				void CConsoleRedirector::f_CloseStdIn()
				{
					if (!mp_hStdinWrite)
						return;

					fp_DestroyHandle(mp_hStdinWrite);
				}

				void CConsoleRedirector::f_SendBinary(NContainer::TCVector<uint8, NMem::CAllocator_HeapSecure> const &_Data)
				{
					if (!mp_hStdinWrite)
						return;
					DWORD dwWritten;
					::WriteFile(mp_hStdinWrite, _Data.f_GetArray(), _Data.f_GetLen(), &dwWritten, nullptr);
				}

				NMib::NProcess::EProcessElevation fg_Process_GetElevation(void *_pProcess)
				{
					using namespace NMib::NSys;
					if (!NMib::NPlatform::fg_IsVista())
						return NMib::NProcess::EProcessElevation_None;

					HANDLE hToken	= nullptr;

					if ( !::OpenProcessToken( 
								_pProcess, 
								TOKEN_QUERY, 
								&hToken ) )
					{
						return NMib::NProcess::EProcessElevation_None;
					}

					DWORD dwReturnLength = 0;

					TOKEN_ELEVATION_TYPE Ret;
					if ( !::GetTokenInformation(
								hToken,
								TokenElevationType,
								&Ret,
								sizeof( Ret ),
								&dwReturnLength ) )
					{
						//ASSERT( FALSE );
						Ret = (TOKEN_ELEVATION_TYPE)0;
					}
					else
					{
						DMibCheck( dwReturnLength == sizeof( Ret ) );
					}

					::CloseHandle( hToken );
					switch (Ret)
					{
					default:
					case TokenElevationTypeDefault:
						return NMib::NProcess::EProcessElevation_None;
					case TokenElevationTypeFull:
						return NMib::NProcess::EProcessElevation_IsElevated;
					case TokenElevationTypeLimited:
						return NMib::NProcess::EProcessElevation_IsNotElevated;
					}
				}

			}
		}
	}
}


void *NMib::NProcess::NPlatform::fg_ProcessLaunch_Open(CProcessLaunchParams const &_Params)
{
	NPtr::TCSharedPointer<CConsoleRedirector> pRedir = fg_Construct();

	pRedir->f_RefCountIncrease(DMibRefcountDebuggingOnly(pRedir->m_DebugSelfRef));

	auto CleanupRef = fg_OnScopeExit
		(
			[&]()
			{
				pRedir->f_RefCountDecrease(DMibRefcountDebuggingOnly(pRedir->m_DebugSelfRef));
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
	pLaunch->f_Start(_Flags);
}

void NMib::NProcess::NPlatform::fg_ProcessLaunch_Close(void *_pLaunch, EProcessLaunchCloseFlag _Flags)
{
	CConsoleRedirector *pLaunch = fg_AutoStaticCast(_pLaunch);
	pLaunch->f_Close(_Flags);
	if (pLaunch->f_RefCountDecrease(DMibRefcountDebuggingOnly(pLaunch->m_DebugSelfRef)) == 0)
		delete pLaunch;
}

bint NMib::NProcess::NPlatform::fg_ProcessLaunch_IsRunning(void *_pLaunch)
{
	CConsoleRedirector *pLaunch = fg_AutoStaticCast(_pLaunch);
	return pLaunch->f_IsRunning();
}

void NMib::NProcess::NPlatform::fg_ProcessLaunch_SendStdIn(void *_pLaunch, NMib::NStr::CStrSecure const &_Data)
{
	CConsoleRedirector *pLaunch = fg_AutoStaticCast(_pLaunch);
	pLaunch->f_SendText(_Data);
}

void NMib::NProcess::NPlatform::fg_ProcessLaunch_CloseStdIn(void *_pLaunch)
{
	CConsoleRedirector *pLaunch = fg_AutoStaticCast(_pLaunch);
	pLaunch->f_CloseStdIn();
}

void NMib::NProcess::NPlatform::fg_ProcessLaunch_SendStdInBinary(void *_pLaunch, NContainer::TCVector<uint8, NMem::CAllocator_HeapSecure> const &_Data)
{
	CConsoleRedirector *pLaunch = fg_AutoStaticCast(_pLaunch);
	pLaunch->f_SendBinary(_Data);
}

fp64 NMib::NProcess::NPlatform::fg_ProcessLaunch_GetRunningTime(void *_pLaunch)
{
	CConsoleRedirector *pLaunch = fg_AutoStaticCast(_pLaunch);
	return pLaunch->f_GetRunningTime();
}

mint NMib::NProcess::NPlatform::fg_ProcessLaunch_GetID(void *_pLaunch)
{
	CConsoleRedirector *pLaunch = fg_AutoStaticCast(_pLaunch);
	return pLaunch->f_GetID();
}
		
void NMib::NProcess::NPlatform::fg_ProcessLaunch_Stop(void *_pLaunch)
{
	CConsoleRedirector *pLaunch = fg_AutoStaticCast(_pLaunch);
	if (!GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, pLaunch->f_GetID()))
	{
		DMibTrace("GenerateConsoleCtrlEvent: {}{\n}", NMib::NPlatform::fg_Win32_GetLastErrorStr());
	}
	//DMibError("Stopping process not implemented");
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
		NMem::fg_MemClear(MemoryInfo);

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
			NTime::CTime ExitTime = NFile::NPlatform::fg_Win32_FileTimeToMalterlibTime(ExitTime1);
			NTime::CTimeSpan KernelTime = NFile::NPlatform::fg_Win32_FileTimeToMalterlibTimeSpan(KernelTime1);
			NTime::CTimeSpan UserTime = NFile::NPlatform::fg_Win32_FileTimeToMalterlibTimeSpan(UserTime1);
			NTime::CTimeSpan RunTime = ExitTime - CreateTime;

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
		
NMib::NProcess::CProcessStatistics NMib::NProcess::NPlatform::fg_ProcessLaunch_GetOverallMemoryStatistics(void *_pLaunch)
{
	CConsoleRedirector *pLaunch = fg_AutoStaticCast(_pLaunch);
	CProcessStatistics Return;
	if (pLaunch->f_GetChildProcess())
	{
		PROCESS_MEMORY_COUNTERS_EX MemoryInfo;
		NMem::fg_MemClear(MemoryInfo);

		if (GetProcessMemoryInfo(pLaunch->f_GetChildProcess(), (PROCESS_MEMORY_COUNTERS *)&MemoryInfo, sizeof(MemoryInfo)))
		{
			Return.m_Statistics("Total page faults", CProcessStat(EProcessStatUnit_GeneralNumber, MemoryInfo.PageFaultCount, 1.0, "faults"));
			Return.m_Statistics("Peak working set size", CProcessStat(EProcessStatUnit_Bytes, MemoryInfo.PeakWorkingSetSize, 1024 * 1024));
			Return.m_Statistics("Peak paged pool usage", CProcessStat(EProcessStatUnit_Bytes, MemoryInfo.QuotaPeakPagedPoolUsage, 1024));
			Return.m_Statistics("Peak non paged pool usage", CProcessStat(EProcessStatUnit_Bytes, MemoryInfo.QuotaPeakNonPagedPoolUsage, 1024));
			Return.m_Statistics("Peak page file usage", CProcessStat(EProcessStatUnit_Bytes, MemoryInfo.PeakPagefileUsage, 1024 * 1024));
		}
	}
	return Return;
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

void NMib::NProcess::NPlatform::fg_Process_RegisterURLHandler(NMib::NStr::CStr const &_Protocol, NMib::NStr::CStr const& _ExePath, NMib::NStr::CStr const &_Params)
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

void NMib::NProcess::NPlatform::fg_Process_RegisterAtStartup(NMib::NStr::CStr const& _ExePath, NMib::NStr::CStr const &_Params, NMib::NStr::CStr const& _Name)
{
	NMib::NPlatform::CWin32_Registry Registry(NMib::NPlatform::CWin32_Registry::ERegRoot_CurrentUser, "Software\\Microsoft\\Windows\\CurrentVersion\\Run");

	NContainer::TCVector<NStr::CStr> lValues;
	Registry.f_EnumValues("", lValues);

	NStr::CStr Name = _Name;
	if (Name.f_IsEmpty())
		Name = fg_GetSys()->f_GetProgramName();
			
	NStr::CStr NamePrefix = Name + "_";
	NStr::CStr CommandLine = _ExePath.f_ReplaceChar('/', '\\') + " " + _Params;

	for(auto VIter = lValues.f_GetIterator(); VIter; ++VIter)
	{
		if ( VIter->f_CmpNoCase(Name) == 0 || VIter->f_StartsWith(NamePrefix) )
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

void NMib::NProcess::NPlatform::fg_Process_DeRegisterAtStartup(NMib::NStr::CStr const& _ExePath, NMib::NStr::CStr const &_Params, NMib::NStr::CStr const& _Name)
{
	NMib::NPlatform::CWin32_Registry Registry(NMib::NPlatform::CWin32_Registry::ERegRoot_CurrentUser, "Software\\Microsoft\\Windows\\CurrentVersion\\Run");

	NContainer::TCVector<NStr::CStr> lValues;
	Registry.f_EnumValues("", lValues);

	NStr::CStr Name = _Name;
	if (Name.f_IsEmpty())
		Name = fg_GetSys()->f_GetProgramName();
	NStr::CStr NamePrefix = Name + "_";
	NStr::CStr CommandLine = _ExePath.f_ReplaceChar('/', '\\') + " " + _Params;

	for(auto VIter = lValues.f_GetIterator(); VIter; ++VIter)
	{
		if ( VIter->f_CmpNoCase(Name) == 0 || VIter->f_StartsWith(NamePrefix) )
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

