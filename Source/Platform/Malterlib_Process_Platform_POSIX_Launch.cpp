// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include "Malterlib_Process_Platform_POSIX_Launch.h"
#include "Malterlib_Process_Platform_POSIX_LaunchLimiter.h"
#include "Malterlib_Process_Platform_POSIX_PlatformSpecific.h"

#include <Mib/Core/PlatformSpecific/PosixFilePath>
#include <Mib/Core/PlatformSpecific/PosixErrNo>
#include <Mib/Core/PlatformSpecific/PosixFork>
#ifdef DPlatformFamily_Linux
#	include <Mib/Core/PlatformSpecific/LinuxOptional>
#endif
#include <Mib/Process/ProcessLaunch>

extern "C"
{
	#include <sys/types.h>
}

#include <sys/resource.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/sysctl.h>

#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <poll.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>

#ifdef DPlatformFamily_OSX
#include <libproc.h>
#endif
#include <sys/ptrace.h>

namespace NMib
{
	namespace NProcess
	{
		namespace NPlatform
		{
			struct CSubSystem_Process_Platform_POSIX_Launch : public CSubSystem
			{
				NThread::CMutual m_LaunchesLock;
				DMibListLinkDS_List(CProcessLaunchLink, m_Link) m_Launches;
				
				~CSubSystem_Process_Platform_POSIX_Launch()
				{
					DMibLock(m_LaunchesLock);
					DMibFastCheck(m_Launches.f_IsEmpty()); // Process launches are still active
				}
				
				void f_DestroyThreadSpecific() override
				{
					fg_ProcessLaunch_CancelAll();
				}
			};
			
			TCSubSystem<CSubSystem_Process_Platform_POSIX_Launch, ESubSystemDestruction_BeforeMemoryManager> g_SubSystem_Process_Platform_POSIX_Launch = {DAggregateInit};

			NStr::CStr fg_FindExecutable(NStr::CStr const &_Path, bint _bAllowLocate, NMib::NFile::EFileAttrib _Type)
			{
				// First look in current dir
				NStr::CStr FullPath = NFile::NPlatform::fg_ConvertToPOSIXPath(_Path, true);
				if (NMib::NFile::CFile::fs_FileExists(FullPath, _Type))
					return FullPath;
				
				if (!_bAllowLocate)
					return _Path;
				
				NStr::CStr PathMixed;
				if (NMib::NSys::fg_Process_GetEnvironmentVariable("PATH", PathMixed))
				{
					NStr::CStr Path = PathMixed;
					
					while (!Path.f_IsEmpty())
					{
						NStr::CStr ThisPath = fg_GetStrSep(Path, ":");
						
						NStr::CStr ExecutablePath = NMib::NFile::CFile::fs_AppendPath(ThisPath, _Path);
						if (NMib::NFile::CFile::fs_FileExists(ExecutablePath, _Type))
							return ExecutablePath;
					}
				}
				return _Path;
			}
			
			
			CPOSIXLaunchContext::CPOSIXLaunchContext()
				: mp_bStarted(false)
				, mp_NeedTermination(EProcessLaunchCloseFlag_None)
				, mp_bClosed(false)
				, mp_bNeedWait(false)
				, mp_ProcessID(-1)
				, m_ExitTime(-1.0)
				, mp_ReturnValue(~uint32(0))
				, mp_hStdinWrite(-1)
				, mp_hStdoutRead(-1)
				, mp_hStderrRead(-1)
				, mp_WakeupPipeRead(-1)
				, mp_WakeupPipeWrite(-1)
			{
				m_TimeSinceStart.f_Start();
				
				auto &SubSystem = *g_SubSystem_Process_Platform_POSIX_Launch;
				{
					DMibLock(SubSystem.m_LaunchesLock);
					SubSystem.m_Launches.f_Insert(this);
				}
			}

			CPOSIXLaunchContext::~CPOSIXLaunchContext()
			{
				fp_Close();
				fp_DestroyPipe(mp_WakeupPipeRead);
				fp_DestroyPipe(mp_WakeupPipeWrite);
				auto &SubSystem = *g_SubSystem_Process_Platform_POSIX_Launch;
				{
					DMibLock(SubSystem.m_LaunchesLock);
					SubSystem.m_Launches.f_Remove(this);
				}
			}
			
			CProcessStatistics CPOSIXLaunchContext::f_OverallMemoryStatistics() const
			{
				DMibLock(mp_OverallStatsLock);
				return mp_OverallMemoryStatistics;
			}
			
			CProcessStatistics CPOSIXLaunchContext::f_OverallExecutionStatistics() const
			{
				DMibLock(mp_OverallStatsLock);
				return mp_OverallExecutionStatistics;
			}
			
			bool CPOSIXLaunchContext::f_RedirectStdInWrite(int &_Pipe, NMib::NStr::CStr &_Errors)
			{	
				if (mp_hStdinWrite != -1)
					fp_DestroyPipe(mp_hStdinWrite );
				mp_hStdinWrite = dup(_Pipe);
				fp_DestroyPipe(_Pipe);
				
				if (mp_hStdinWrite == -1)
				{
					_Errors += NMib::NPlatform::fg_FormatErrno("dup2 (launch process stdin pipe)", errno);
					return false;
				}
				
				return true;
			}

			void CPOSIXLaunchContext::fp_DestroyPipe(int &_Handle)
			{
				if (_Handle != -1)
				{
					close(_Handle);
					_Handle = -1;
				}
			}
			
			NStr::CStr CPOSIXLaunchContext::f_GetThreadName()
			{
				return "CPOSIXLaunchContext";
			}

			bool CPOSIXLaunchContext::f_DestroyThread()
			{
				if (f_RefCountDecrease() == 0)
				{
					delete this;
					return true;
				}
				return false;
			}

			bint CPOSIXLaunchContext::fp_LaunchChild
				(
					int &_hStdOutRead
					, int &_hStdOutWrite
					, int &_hStdInRead
					, int &_hStdInWrite
					, int &_hStdErrRead
					, int &_hStdErrWrite
					, NStr::CStr &_Errors
				)
			{
				
				
				
				NStr::CStr Program = mp_LastLaunchOptions.m_Target;
				
#ifdef DPlatformFamily_Linux
				
				if (mp_LastLaunchOptions.m_LaunchType == NProcess::EProcessLaunchType_Document
					||	mp_LastLaunchOptions.m_LaunchType == NProcess::EProcessLaunchType_URL )
				{					
					if (!fg_Linux_LaunchDocumentOrURL(mp_LastLaunchOptions, Program, _Errors))
					{
						return false;
					}
				}
				else if (mp_LastLaunchOptions.m_Elevation == NProcess::EProcessLaunchElevation_Elevate)
				{
					if (!fg_Linux_LaunchExecutableWithRoot(this, mp_LastLaunchOptions, Program, _hStdErrRead, _hStdErrWrite, _hStdInRead, _hStdInWrite, _Errors))
					{
						return false;
					}
				}
#endif
				
#ifndef DPlatformFamily_Linux
				
				if (		mp_LastLaunchOptions.m_LaunchType == NProcess::EProcessLaunchType_Document
							||	mp_LastLaunchOptions.m_LaunchType == NProcess::EProcessLaunchType_URL ) 
				{
					
					if (mp_LastLaunchOptions.m_bSandboxed)
					{
						_Errors += "Launching sandboxed not supported for document/URL launches implemented\n";
						return false;
					}
					
#ifdef DPlatformFamily_OSX
					if (mp_LastLaunchOptions.m_Operation == "open folder")
					{
						return fg_MacOSX_LaunchFinder(mp_LastLaunchOptions, mp_ProcessID, _Errors);
					}
					
					bint bRet = fg_MacOSX_LaunchDocument(mp_LastLaunchOptions, mp_ProcessID, _Errors);
					return bRet;
					
					_Errors += "Launching document is not implemented\n";
					return false;				
#endif
				}
				else
					
#endif
					
				if (mp_LastLaunchOptions.m_Elevation == EProcessLaunchElevation_Elevate)
				{
					if (mp_LastLaunchOptions.m_bSandboxed)
					{
						_Errors += "Launching sandboxed not supported for elevation launches implemented\n";
						return false;
					}
#ifdef DPlatformFamily_OSX
					fp_DestroyPipe(_hStdErrWrite);
					fp_DestroyPipe(_hStdErrRead);
					
					bint bRetVal = fg_MacOSX_LaunchExecutableWithRoot(mp_LastLaunchOptions, mp_ProcessID, _hStdOutRead, _hStdInWrite, _Errors);
					
					if (bRetVal)
					{
						mp_hStdinWrite = _hStdInWrite;
						mp_hStdoutRead = _hStdOutRead;
						_hStdInWrite = -1;
						_hStdOutRead = -1;
						fp_DestroyPipe(_hStdInRead);
						fp_DestroyPipe(_hStdOutWrite);
					}

					
					return bRetVal;
#else
					_Errors += "Launching elevated is not implemented" DMibNewLine;
					return false;
#endif
				}
				else if (mp_LastLaunchOptions.m_Elevation == EProcessLaunchElevation_DeElevate)
				{
					if (mp_LastLaunchOptions.m_bSandboxed)
					{
						_Errors += "Launching sandboxed not supported for de-elevation launches implemented\n";
						return false;
					}
					_Errors += "Launching de-elevated is not implemented\n";
					return false;
				}
				else
				{
					

#ifdef DPlatformFamily_OSX
					NStr::CStr OriginalProgram = Program;
					Program = fg_FindExecutable(Program, mp_LastLaunchOptions.m_bAllowExecutableLocate, NMib::NFile::EFileAttrib_File | NMib::NFile::EFileAttrib_Directory);
					if ( NMib::NFile::CFile::fs_FileExists(Program, NMib::NFile::EFileAttrib_Directory))
					{
						if (NMib::NFile::CFile::fs_GetExtension(Program).f_CmpNoCase("app") == 0)
						{
							if (mp_LastLaunchOptions.m_bSandboxed)
							{
								_Errors += "Launching sandboxed not supported for app-bundle launches implemented\n";
								return false;
							}
							auto NewLaunchOptions = mp_LastLaunchOptions;
							NewLaunchOptions.m_Target = Program;
							return fg_MacOSX_LaunchUIExecutable(NewLaunchOptions, mp_ProcessID, _Errors);
						}
						else
							Program = fg_FindExecutable(OriginalProgram, mp_LastLaunchOptions.m_bAllowExecutableLocate, NMib::NFile::EFileAttrib_File | NMib::NFile::EFileAttrib_Executable);
					}
#else
					Program = fg_FindExecutable(Program, mp_LastLaunchOptions.m_bAllowExecutableLocate, NMib::NFile::EFileAttrib_File | NMib::NFile::EFileAttrib_Executable);
#endif

					mp_ProcessID = -1;
					
					
					uid_t RunAsUser = -1;
					gid_t RunAsGroup = -1;
					
					try
					{
						if (!mp_LastLaunchOptions.m_RunAsUser.f_IsEmpty())
						{
							NStr::CStr UserID;
							if (!NSys::fg_UserManagement_UserExists(mp_LastLaunchOptions.m_RunAsUser, UserID))
							{
								_Errors += NMib::NStr::CStr::CFormat("User '{}' does not exist") << mp_LastLaunchOptions.m_RunAsUser;
								return false;
							}
							RunAsUser = UserID.f_ToInt(uid_t(-1));
						}
						if (!mp_LastLaunchOptions.m_RunAsGroup.f_IsEmpty())
						{
							NStr::CStr GroupID;
							if (!NSys::fg_UserManagement_GroupExists(mp_LastLaunchOptions.m_RunAsGroup, GroupID))
							{
								_Errors += NMib::NStr::CStr::CFormat("Group '{}' does not exist") << mp_LastLaunchOptions.m_RunAsGroup;
								return false;
							}
							RunAsGroup = GroupID.f_ToInt(gid_t(-1));
						}
					}
					catch (NException::CException const &_Exception)
					{
						_Errors += NMib::NStr::CStr::CFormat("Error getting user/group ID to run as: {}\n") << _Exception.f_GetErrorStr();
						return false;
					}
					

					NStr::CStr Chroot;
					
					auto fl_ConvertChrootPath
						= [&](NStr::CStr const &_Path) -> NStr::CStr
						{
							NStr::CStr CanonicalPath = _Path;
							if (Chroot.f_IsEmpty())
								return CanonicalPath;
							if (_Path.f_StartsWith(Chroot))
							{
								NStr::CStr Return = CanonicalPath.f_Extract(Chroot.f_GetLen());
								if (Return.f_IsEmpty())
									Return = "/";
								DMibTrace("Chroot path: {} -> {}\n", CanonicalPath << Return);
								return Return;
							}
							DMibTrace("Chroot path: {}\n", CanonicalPath);
							return CanonicalPath;
						}
					;
					
					if (mp_LastLaunchOptions.m_bSandboxed)
					{
						auto pRoot = mp_LastLaunchOptions.m_SandboxRoots.f_FindEqual(NStr::CStr());
						if (pRoot)
							Chroot = *pRoot;
					}
					
					NStr::CStr ProgramToLaunch = fl_ConvertChrootPath(Program);

					NContainer::TCVector<NStr::CStr> Parameters;
					NContainer::TCVector<ch8 *> ParametersList;
					
					ParametersList.f_Insert(ProgramToLaunch.f_GetStrUniqueWritable());
					NStr::CStr Params = mp_LastLaunchOptions.m_Parameters;
					while (!Params.f_IsEmpty())
					{
						NStr::CStr Param = fg_GetStrSepEscaped(Params, " ");
						ParametersList.f_Insert(Parameters.f_Insert(Param).f_GetStrUniqueWritable());
					}
					
					ParametersList.f_Insert((ch8 *)nullptr);

					DMibSafeCheck(mp_ProcessID == -1, "Error");

					NContainer::TCVector<NStr::CStr> Env;
					NContainer::TCVector<ch8 *> EnvList;
					auto FinalEnv = NMib::NSys::fg_Process_GetEnvironmentVariables();
					if (!mp_LastLaunchOptions.m_Environment.f_IsEmpty())
					{
						NContainer::TCMap<NStr::CStr, NStr::CStr> NewEnvironment;
						if (mp_LastLaunchOptions.m_bMergeEnvironment)
						{
							auto OldEnv = fg_Move(FinalEnv);
							FinalEnv = mp_LastLaunchOptions.m_Environment;
							FinalEnv += OldEnv;
						}
						else
							FinalEnv = mp_LastLaunchOptions.m_Environment;
					}
					
					for (auto iEnv = FinalEnv.f_GetIterator(); iEnv; ++iEnv)
					{
						EnvList.f_Insert(Env.f_Insert(NStr::CStr(NStr::CStr::CFormat("{}={}") << iEnv.f_GetKey() << *iEnv)).f_GetStrUniqueWritable());
					}
					
					EnvList.f_Insert((ch8 *)nullptr);

					//DMibLock(m_ForkLock); // To protect 

#ifdef DPlatformFamily_OSX
					DMibLock(NMib::NPlatform::fg_ForkLock());
#endif
					
					if (fg_GetSys()->f_IsDll()) // We need to prepare
						NMib::NPlatform::fg_ForkPrepare();
					
					pid_t ForkResult = fork();
					
					if (fg_GetSys()->f_IsDll()) // We need to cleanup after fork
						NMib::NPlatform::fg_ForkParentOrChild();

					if (ForkResult == -1)
					{
						int ErrNo = errno;
						_Errors += NMib::NPlatform::fg_FormatErrno("fork (launch process)", ErrNo);
						_Errors += "\n";
						return false;
					}
					else if (ForkResult == 0)
					{
						// 0 = stdin
						// 1 = stdout
						// 2 = stderr
						
						try
						{
							fp_DestroyPipe(_hStdInWrite);
							if (_hStdInRead == -1)
								close(0);
							else
								dup2(_hStdInRead, 0);
							
							fp_DestroyPipe(_hStdOutRead);
							if (_hStdOutWrite == -1)
								close(1);
							else
								dup2(_hStdOutWrite, 1);
							
							fp_DestroyPipe(_hStdErrRead);
							if (_hStdErrWrite == -1)
							{
								if (_hStdOutWrite != -1)
									dup2(_hStdOutWrite, 2);
								else
									close(2);
							}
							else
								dup2(_hStdErrWrite, 2);

							fp_DestroyPipe(_hStdInRead);
							fp_DestroyPipe(_hStdOutWrite);
							fp_DestroyPipe(_hStdErrWrite);

							if (mp_LastLaunchOptions.m_LaunchPriority != EExecutionPriority_Default)
								fg_Process_SetPriority(mp_LastLaunchOptions.m_LaunchPriority);
							
							for (auto iLimit = mp_LastLaunchOptions.m_Limits.f_GetIterator(); iLimit; ++iLimit)
							{
								int RLimit = 0;
								switch (iLimit.f_GetKey())
								{
								case EProcessLimit_CoreDumpSize: RLimit = RLIMIT_CPU; break;
								case EProcessLimit_CpuTime: RLimit = RLIMIT_CPU; break;
								case EProcessLimit_DataSegment: RLimit = RLIMIT_DATA; break;
								case EProcessLimit_FileSize: RLimit = RLIMIT_FSIZE; break;
								case EProcessLimit_LockedMemory: RLimit = RLIMIT_MEMLOCK; break;
								case EProcessLimit_OpenedFiles: RLimit = RLIMIT_NOFILE; break;
								case EProcessLimit_Threads: RLimit = RLIMIT_NPROC; break;
								case EProcessLimit_ResidentMemory: RLimit = RLIMIT_RSS; break;
								case EProcessLimit_StackSize: RLimit = RLIMIT_STACK; break;
								
	#ifdef DPlatformFamily_Linux
								case EProcessLimit_VirtualAddressSpace: RLimit = RLIMIT_AS; break;
								case EProcessLimit_MessageQueueSize: RLimit = RLIMIT_MSGQUEUE; break;
								case EProcessLimit_NiceValue: RLimit = RLIMIT_NICE; break;
								case EProcessLimit_RealtimePriority: RLimit = RLIMIT_RTPRIO; break;
								case EProcessLimit_RealtimeTime: RLimit = RLIMIT_RTTIME; break;
								case EProcessLimit_SignalsPending: RLimit = RLIMIT_SIGPENDING; break;
	#endif
								default:
									{
										
										DMibConErrOut("Unsupported limit {} when setting limits in forked process{\n}", iLimit.f_GetKey());
										NMib::NSys::fg_TerminateProcess(67);
									}
									break;
								}

								rlimit Limits;
								if (!getrlimit(RLimit, &Limits))
								{
									if (!iLimit->f_IsMaxUnchaned())
									{
										if (iLimit->f_IsMaxUnlimited())
											Limits.rlim_max = RLIM_INFINITY;
										else
											Limits.rlim_max = iLimit->m_MaxValue;
									}
									if (!iLimit->f_IsUnchaned())
									{
										if (iLimit->f_IsUnlimited())
											Limits.rlim_cur = RLIM_INFINITY;
										else
											Limits.rlim_cur = iLimit->m_Value;
									}
									if (setrlimit(RLimit, &Limits))
									{
										int ErrNo = errno;
										DMibConErrOut("{}{\n}", NMib::NPlatform::fg_FormatErrno(NStr::CStr::CFormat("setrlimit({}) when setting limits in forked process") << RLimit, ErrNo));
										NMib::NSys::fg_TerminateProcess(67);
									}
								}
								else
								{
									int ErrNo = errno;
									DMibConErrOut("{}{\n}", NMib::NPlatform::fg_FormatErrno(NStr::CStr::CFormat("getrlimit({}) when setting limits in forked process") << RLimit, ErrNo));
									NMib::NSys::fg_TerminateProcess(67);
								}
							}

							// Group needs to be set first as permissions to set user will be lost
							if (mp_LastLaunchOptions.m_bMakeEffectiveGroupReal)
								setgid(getegid());
							else if (RunAsGroup != gid_t(-1))
							{
								if (setgid(RunAsGroup))
								{
									int ErrNo = errno;
									DMibConErrOut("{}{\n}", NMib::NPlatform::fg_FormatErrno(NStr::CStr::CFormat("setgid({}) when setting process group in forked process") << RunAsGroup, ErrNo));
									NMib::NSys::fg_TerminateProcess(65);
								}
							}
							
							if (mp_LastLaunchOptions.m_bMakeEffectiveUserReal)
								setuid(geteuid());
							else if (RunAsUser != uid_t(-1))
							{
								if (setuid(RunAsUser))
								{
									int ErrNo = errno;
									DMibConErrOut("{}{\n}", NMib::NPlatform::fg_FormatErrno(NStr::CStr::CFormat("setuid({}) when setting process user in forked process") << RunAsUser, ErrNo));
									NMib::NSys::fg_TerminateProcess(64);
								}
							}
							
							if (!Chroot.f_IsEmpty())
							{
								NStr::CStr LaunchHelper = fg_FindExecutable("MalterlibSandBox_" DMibStringize(DArchitecture), true, NMib::NFile::EFileAttrib_File);
								
								if (!NMib::NFile::CFile::fs_FileExists(LaunchHelper, NMib::NFile::EFileAttrib_File))
								{
									DMibConErrOut("Could not find MalterlibSandBox_" DMibStringize(DArchitecture) " in program directory" DMibNewLine, 0);
									NMib::NSys::fg_TerminateProcess(1);
								}
								
								NStr::CStr WorkingDir;
								if (!mp_LastLaunchOptions.m_WorkingDirectory.f_IsEmpty())
									WorkingDir = fl_ConvertChrootPath(mp_LastLaunchOptions.m_WorkingDirectory);
								else
									WorkingDir = fl_ConvertChrootPath(NMib::NFile::CFile::fs_GetCurrentDirectory());
								
								NContainer::TCVector<ch8 *> NewParametersList;
								
								NewParametersList.f_Insert(LaunchHelper.f_GetStrUniqueWritable());
								NewParametersList.f_Insert(Chroot.f_GetStrUniqueWritable());
								NewParametersList.f_Insert(WorkingDir.f_GetStrUniqueWritable());
								if (mp_LastLaunchOptions.m_bCopyRootToSandbox)
									NewParametersList.f_Insert((char *)"1");
								else
									NewParametersList.f_Insert((char *)"0");

								NewParametersList.f_Insert(ParametersList);

								execve(LaunchHelper, NewParametersList.f_GetArray(), EnvList.f_GetArray());
								int ErrNo = errno;
								DMibConErrOut("{}{\n}", NMib::NPlatform::fg_FormatErrno(NStr::CStr::CFormat("execve({}) when executing in forked chrooted process") << ProgramToLaunch, ErrNo));
								NMib::NSys::fg_TerminateProcess(1);
								
							}
							else
							{
								if (!mp_LastLaunchOptions.m_WorkingDirectory.f_IsEmpty())
									NSys::NFile::fg_SetCurrentDirectory(fl_ConvertChrootPath(mp_LastLaunchOptions.m_WorkingDirectory));
							}
							
							execve(ProgramToLaunch.f_GetStr(), ParametersList.f_GetArray(), EnvList.f_GetArray());
							int ErrNo = errno;
							DMibConErrOut("{}{\n}", NMib::NPlatform::fg_FormatErrno(NStr::CStr::CFormat("execve({}) when executing in forked process") << ProgramToLaunch, ErrNo));
							NMib::NSys::fg_TerminateProcess(1);
						}
						catch (NException::CException const &_Exception)
						{
							DMibConErrOut("Error setting up for fork: {}{\n}", _Exception.f_GetErrorStr());
							NMib::NSys::fg_TerminateProcess(1);
						}
					}
					else
					{

						mp_hStdinWrite = _hStdInWrite;
						mp_hStdoutRead = _hStdOutRead;
						mp_hStderrRead = _hStdErrRead;
						_hStdInWrite = -1;
						_hStdOutRead = -1;
						_hStdErrRead = -1;
						fp_DestroyPipe(_hStdInRead);
						fp_DestroyPipe(_hStdOutWrite);
						fp_DestroyPipe(_hStdErrWrite);
						
						mp_ProcessID = ForkResult;
					}
					return true;
				}
				return false;
			}
			
			void CPOSIXLaunchContext::fp_Close()
			{
				fp_DestroyPipe(mp_hStdinWrite);
				fp_DestroyPipe(mp_hStdoutRead);
				fp_DestroyPipe(mp_hStderrRead);
			}
			
			bool fg_CreatePipe(int &_Read, int &_Write)
			{
				DMibFastCheck(_Read == -1);
				DMibFastCheck(_Write == -1);
				int Pipes[2];
#ifndef DPlatformFamily_OSX
				if (NLocal::g_f_pipe2)
				{
					if (NLocal::g_f_pipe2(Pipes, O_CLOEXEC))
						return false;
				}
				else
#endif
				{
					// We need to make sure that we protect the pipes against being included in other processes
					DMibLock(NMib::NPlatform::fg_ForkLock());
					if (pipe(Pipes))
						return false;

					fcntl(Pipes[0], F_SETFD, fcntl(Pipes[0], F_GETFD) | FD_CLOEXEC);
					fcntl(Pipes[1], F_SETFD, fcntl(Pipes[1], F_GETFD) | FD_CLOEXEC);
				}
				
#ifdef F_SETNOSIGPIPE
				fcntl(Pipes[0], F_SETNOSIGPIPE, 1);
				fcntl(Pipes[1], F_SETNOSIGPIPE, 1);
#endif

				_Read = Pipes[0];
				_Write = Pipes[1];
				return true;
			}
			
			bint CPOSIXLaunchContext::fp_DoStart(NStr::CStr &_Errors)
			{
				
				fp_Close();
				bool bOK = false;
				
				int hStdoutRead;				// parent stdout read int
				int hStdoutWrite;	// child stdout write int
				int hStderrRead;				// parent stdout read int
				int hStderrWrite;	// child stdout write int
				int hStdinWrite;				// parent stdin write int
				int hStdinRead;					// child stdin read int
				
				hStdoutRead = -1;
				hStdoutWrite = -1;
				hStderrRead = -1;
				hStderrWrite = -1;
				hStdinWrite = -1;
				hStdinRead = -1;
				
				do
				{
					// Create a child stdout pipe.
					if (mp_LastLaunchOptions.m_bEnableStdRedirection)
					{
						
						if (!fg_CreatePipe(hStdoutRead, hStdoutWrite))
						{
							int ErrNo = errno;
							_Errors += NMib::NPlatform::fg_FormatErrno("pipe (launch process stdout)", ErrNo);
							_Errors += "\n";
							break;
						}
						
						if (mp_LastLaunchOptions.m_bSeparateStdErr)
						{
							if (!fg_CreatePipe(hStderrRead, hStderrWrite))
							{
								int ErrNo = errno;
								_Errors += NMib::NPlatform::fg_FormatErrno("pipe (launch process stderr)", ErrNo);
								_Errors += "\n";
								break;
							}
						}
						
						// Create a child stdin pipe.
						if (!fg_CreatePipe(hStdinRead, hStdinWrite))
						{
							int ErrNo = errno;
							_Errors += NMib::NPlatform::fg_FormatErrno("pipe (launch process stdin)", ErrNo);
							_Errors += "\n";
							break;
						}
						
					}
					else
					{
						// We need this redirection for checking for process exit
						if (!fg_CreatePipe(hStdoutRead, hStdoutWrite))
						{
							int ErrNo = errno;
							_Errors += NMib::NPlatform::fg_FormatErrno("pipe (launch process check exit, stdout)", ErrNo);
							_Errors += "\n";
							break;
						}
					}

					if (hStdoutRead != -1)
						fcntl(hStdoutRead, F_SETFL, fcntl(hStdoutRead, F_GETFL) | O_NONBLOCK);
					if (hStderrRead != -1)
						fcntl(hStderrRead, F_SETFL, fcntl(hStderrRead, F_GETFL) | O_NONBLOCK);
					
					// launch the child process
					if (!fp_LaunchChild(hStdoutRead, hStdoutWrite, hStdinRead, hStdinWrite, hStderrRead, hStderrWrite, _Errors))
						break;
					
					// Child is launched. Close the parents copy of those pipe
					// ints that only the child should have open.
					// Make sure that no ints to the write end of the stdout pipe
					// are maintained in this process or else the pipe will not
					// close when the child process exits and ReadFile will hang.
					
					bOK = true;
				}
				while (0);
				
				fp_DestroyPipe(hStdoutRead);
				fp_DestroyPipe(hStdoutWrite);
				fp_DestroyPipe(hStderrRead);
				
				fp_DestroyPipe(hStderrWrite);
				fp_DestroyPipe(hStdinWrite);
				fp_DestroyPipe(hStdinRead);
				
				if (!bOK)
					fp_Close();
				
				return bOK;
			}
			
			void CPOSIXLaunchContext::fp_RedirectOutput(bool _bWaitForEOF)
			{
//				DMibScopeConOutTimer("fp_RedirectOutput");
				auto fl_Read
					= [&](int _FileDes, EProcessLaunchOutputType _OutputType) -> bool
					{
						bool bEndOfFile = false;
						while (1)
						{
							{
								//DMibScopeConOutTimer("AllRead");
								uint8 Buffer[4096];
								
								ssize_t ReadBytes;
								ReadBytes = read(_FileDes, Buffer, 4096);
								
								if (ReadBytes == -1)
								{
									int ErrNo = errno;
									if (ErrNo == EAGAIN)
										break;
									fp_OnOutput(EProcessLaunchOutputType_GeneralError, NMib::NPlatform::fg_FormatErrno("read (process launch redirect output)", ErrNo) + "\n");
									break;
								}
								else if (ReadBytes == 0)
								{
									bEndOfFile = true;
									break;
								}
								if (mp_LastLaunchOptions.m_bEnableStdRedirection)
								{
									NStr::CStr Temp;
									Temp.f_AddStr((ch8 const *)Buffer, ReadBytes);
									//DMibConOut("{}", Temp);
									fp_OnOutput(_OutputType, Temp);
								}
							}
						}
						return bEndOfFile;
					}
				;
				
				if (_bWaitForEOF)
				{
					pollfd ToPoll[2] = {0};
					bool bStdOutEof = mp_hStdoutRead == -1;
					bool bStdErrEof = mp_hStderrRead == -1;
					while (!bStdOutEof || !bStdErrEof)
					{
						int nPoll = 0;
						int iStdOutPoll = -1;
						if (!bStdOutEof)
						{
							iStdOutPoll = nPoll;
							ToPoll[nPoll].fd = mp_hStdoutRead;
							ToPoll[nPoll].events = POLLRDNORM;
							ToPoll[nPoll].revents = 0;
							++nPoll;
						}
						int iStdErrPoll = -1;
						if (!bStdErrEof)
						{
							iStdErrPoll = nPoll;
							ToPoll[nPoll].fd = mp_hStderrRead;
							ToPoll[nPoll].events = POLLRDNORM;
							ToPoll[nPoll].revents = 0;
							++nPoll;
						}
						
						int PollReturn = poll(ToPoll, nPoll, -1);
						if (PollReturn == -1)
						{
							int ErrNo = errno;
							if (ErrNo != EINTR)
							{
								fp_OnOutput(EProcessLaunchOutputType_GeneralError, NMib::NPlatform::fg_FormatErrno(NStr::CFStr256::CFormat("poll ({}, launch process)") << nPoll, ErrNo) + "\n");
								break;
							}
						}
						if (!bStdOutEof)
						{
							if (fl_Read(mp_hStdoutRead, EProcessLaunchOutputType_StdOut))
								bStdOutEof = true;							
						}
						if (!bStdErrEof)
						{
							if (fl_Read(mp_hStderrRead, EProcessLaunchOutputType_StdErr))
								bStdErrEof = true;							
						}
					}
					
				}
				else
				{
					if (mp_hStdoutRead != -1)
					{
						if (fl_Read(mp_hStdoutRead, EProcessLaunchOutputType_StdOut))
							fp_DestroyPipe(mp_hStdoutRead); // End of file reached
					}
					if (mp_hStderrRead != -1)
					{
						if (fl_Read(mp_hStderrRead, EProcessLaunchOutputType_StdErr))
							fp_DestroyPipe(mp_hStderrRead); // End of file reached
					}
				}
			}

			bool fg_TerminateProcess(pid_t _ProcessID, NStr::CStr &_Errors)
			{
				if (kill(_ProcessID, SIGKILL))
				{
					int ErrNo = errno;
					_Errors += NMib::NPlatform::fg_FormatErrno(NStr::CStr::CFormat("kill({}) when terminating launched process") << _ProcessID, ErrNo);
					return false;
				}
				return true;
			}
			
			bool fg_StopProcess(pid_t _ProcessID, NStr::CStr &_Errors)
			{
				if (kill(_ProcessID, SIGTERM))
				{
					int ErrNo = errno;
					_Errors += NMib::NPlatform::fg_FormatErrno(NStr::CStr::CFormat("kill({}) when stopping launch process") << _ProcessID, ErrNo);
					return false;
				}
				return true;
			}
			
			bool fg_TerminateProcessTree(pid_t _ProcessID, NStr::CStr &_Errors)
			{
#ifdef DPlatformFamily_OSX
				return fg_MacOSX_Process_TerminateTree(_ProcessID, _Errors);
#elif defined(DPlatformFamily_Linux)
				return fg_Linux_Process_TerminateTree(_ProcessID, _Errors);
#else
				_Errors += "Kill process tree not implemented for this platform";
				return false;
#endif
			}
			
			aint CPOSIXLaunchContext::f_Main()
			{
				NStr::CStr Errors;
				if (!fp_DoStart(Errors))
				{
					fp_OnLaunched(Errors, nullptr, false);
					return 0;
				}

				fp_OnLaunched(Errors, (void *)(mint)mp_ProcessID, true);
				
				bool bExited = false;
				
				int ExitCode = 255;
				
				if (mp_ProcessID == -1)
				{
					bExited = true;
				}
				else
				{
					if (mp_LastLaunchOptions.m_CPUUsage != 0.0 && mp_LastLaunchOptions.m_CPUUsage != 1.0)
						mp_pCPULimiter = fg_GetCPULimiter(mp_LastLaunchOptions.m_ProcessGroup, mp_LastLaunchOptions.m_CPUUsage, mp_ProcessID);
					
#ifdef DPlatformFamily_OSX
					proc_taskallinfo TaskInfoAll = {0};
#endif
					
					// Make sure we are non-blocking
					if (mp_hStdoutRead != -1)
						fcntl(mp_hStdoutRead, F_SETFL, fcntl(mp_hStdoutRead, F_GETFL) | O_NONBLOCK);
					if (mp_hStderrRead != -1)
						fcntl(mp_hStderrRead, F_SETFL, fcntl(mp_hStderrRead, F_GETFL) | O_NONBLOCK);

					pollfd ToPoll[3] = {0};
					
					while (true)
					{
						int nPoll = 0;
						int iWakeupPoll = nPoll;
						ToPoll[nPoll].fd = mp_WakeupPipeRead;
						ToPoll[nPoll].events = POLLRDNORM;
						ToPoll[nPoll].revents = 0;
						++nPoll;
						int iStdOutPoll = -1;
						if (mp_hStdoutRead != -1)
						{
							iStdOutPoll = nPoll;
							ToPoll[nPoll].fd = mp_hStdoutRead;
							ToPoll[nPoll].events = POLLRDNORM;
							ToPoll[nPoll].revents = 0;
							++nPoll;
						}
						int iStdErrPoll = -1;
						if (mp_hStderrRead != -1)
						{
							iStdErrPoll = nPoll;
							ToPoll[nPoll].fd = mp_hStderrRead;
							ToPoll[nPoll].events = POLLRDNORM;
							ToPoll[nPoll].revents = 0;
							++nPoll;
						}
						
						int PollReturn;
						if (nPoll == 1)
							PollReturn = poll(ToPoll, nPoll, 1); // In this case we have no way to get a notification when process exits
						else
							PollReturn = poll(ToPoll, nPoll, -1);
						
						if (PollReturn == -1)
						{
							int ErrNo = errno;
							if (ErrNo != EINTR)
							{
								fp_OnOutput(EProcessLaunchOutputType_GeneralError, NMib::NPlatform::fg_FormatErrno(NStr::CFStr256::CFormat("poll ({}, launch process)") << nPoll, ErrNo) + "\n");
								bExited = true;
								break;
							}
						}
											
						fp_RedirectOutput(false);						

						// You have to get proc info before wait4 as the zombie process will be invalid after it
#ifdef DPlatformFamily_OSX
						int bytes = 0;
						bytes = proc_pidinfo(mp_ProcessID, PROC_PIDTASKINFO, 0, &TaskInfoAll.ptinfo, PROC_PIDTASKINFO_SIZE );
						
						bool bAskForZombie = true;
						
						if (NMib::CSystem::ms_PlatformVersion >= 10'05'00 && NMib::CSystem::ms_PlatformVersion < 10'06'00)
							bAskForZombie = false; // On OSX 10.5 asking for a zombie process will cause a kernel panic
						
						bytes = proc_pidinfo(mp_ProcessID, PROC_PIDTBSDINFO, bAskForZombie, &TaskInfoAll.pbsd, PROC_PIDTBSDINFO_SIZE);
//						bytes = proc_pidinfo(mp_ProcessID, PROC_PIDTASKALLINFO, 1, &TaskInfoAll, sizeof(TaskInfoAll));

						if (bytes <= 0)
						{
							int ErrNo = errno;
							(void)ErrNo;
//							DMibDTrace("proc_pidinfo {}\n", NMib::NPlatform::fg_FormatErrno("", ErrNo));
							
						}
						else if (bytes < PROC_PIDTBSDINFO_SIZE)
						{
							int ErrNo = errno;
							(void)ErrNo;
//							DMibDTrace("proc_pidinfo {} < {} = {}\n", bytes << (sizeof(TaskInfoAll)) << NMib::NPlatform::fg_FormatErrno("", ErrNo));
						}
#endif
						
						rusage RUsage;
						int Status = 0;
						int WaitResult = wait4(mp_ProcessID, &Status, WNOHANG, &RUsage);
						
						if (WaitResult == -1)
						{
							int ErrNo = errno;
							bExited = true;
							fp_OnOutput(EProcessLaunchOutputType_GeneralError, NMib::NPlatform::fg_FormatErrno("waitpid (launch process)", ErrNo) + "\n");
							break;
						}
						
						if (WaitResult != 0)
						{
							fp_UpdateOverallStats
								(
									RUsage
#ifdef DPlatformFamily_OSX
									, TaskInfoAll
#endif
								)
							;
							
							if (WIFEXITED(Status))
							{
								ExitCode = WEXITSTATUS(Status);
								bExited = true;
								break;
							}
							else if (WIFSIGNALED(Status))
							{
								int Signal = WTERMSIG(Status);
								bint bCoreDumped = WCOREDUMP(Status);
								bExited = true;
								fp_OnOutput
									(
										EProcessLaunchOutputType_GeneralError
										, NStr::CStr::CFormat("Process terminated due to signal {}{}\n") << Signal << (bCoreDumped ? " and a core dump was created\n" : "\n")
									)
								;
								break;
							}
							else if (WIFSTOPPED(Status))
							{
								int Signal = WSTOPSIG(Status);
								bExited = true;
								fp_OnOutput(EProcessLaunchOutputType_GeneralError, NStr::CStr::CFormat("Process stopped due to signal {}\n") << Signal);
								break;
							}
							else
							{
								bExited = true;
								fp_OnOutput(EProcessLaunchOutputType_GeneralError, NStr::CStr::CFormat("Unknown exit reason {}\n") << Status);
								break;
							}
						}

						if (ToPoll[iWakeupPoll].revents & POLLRDNORM)
						{
							while (1)
							{
								{
									//DMibScopeConOutTimer("AllRead");
									uint8 Buffer[16];
									
									ssize_t ReadBytes;
									ReadBytes = read(mp_WakeupPipeRead, Buffer, 16);
									
									if (ReadBytes == -1)
										break;
									else if (ReadBytes == 0)
										break;
								}
							}
							EProcessLaunchCloseFlag NeedTermination = EProcessLaunchCloseFlag_None;
							bint bNeedWait = false;
							bint bClosed = false;
							{
								DMibLock(mp_NeedTerminationLock);
								NeedTermination = mp_NeedTermination;
								bNeedWait = mp_bNeedWait;
								bClosed = mp_bClosed;
							}

							if (bClosed)
							{
								if (NeedTermination & EProcessLaunchCloseFlag_TerminateProcess)
								{
									NStr::CStr TempRet;
									if (mp_ProcessID)
									{
										if (mp_LastLaunchOptions.m_bSandboxed)
										{
											if (!fg_TerminateProcessTree(mp_ProcessID, TempRet))
												bNeedWait = false;
										}
										else
										{
											if (!fg_TerminateProcess(mp_ProcessID, TempRet))
												bNeedWait = false;
										}
									}
									else
									{
										fp_OnOutput(EProcessLaunchOutputType_TerminateMessage, "Cannot terminate process, because no process ID is available");
										bNeedWait = false;
									}
									if (!TempRet.f_IsEmpty())
										fp_OnOutput(EProcessLaunchOutputType_TerminateMessage, TempRet);
								}
								else if (NeedTermination & EProcessLaunchCloseFlag_StopProcess)
								{
									NStr::CStr TempRet;
									if (mp_ProcessID)
									{
										if (!fg_StopProcess(mp_ProcessID, TempRet))
											bNeedWait = false;
									}
									else
									{
										fp_OnOutput(EProcessLaunchOutputType_TerminateMessage, "Cannot stop process, because no process ID is available");
										bNeedWait = false;
									}
									if (!TempRet.f_IsEmpty())
										fp_OnOutput(EProcessLaunchOutputType_TerminateMessage, TempRet);
								}
								if (!bNeedWait)
									break;
							}
						}
					}
					mp_pCPULimiter.f_Clear();
				}

/*				if (mp_hStdoutRead != -1)
					fcntl(mp_hStdoutRead, F_SETFL, 0);
				if (mp_hStderrRead != -1)
					fcntl(mp_hStderrRead, F_SETFL, 0);*/
				fp_RedirectOutput(bExited);

				if (bExited)
				{
					if (mp_LastLaunchOptions.m_fOnStateChange)
					{
						fp_OnExit(ExitCode);
					}
				}
				fp_Close();
				return 0;
			}
			
			void CPOSIXLaunchContext::fp_OnLaunched(NMib::NStr::CStr const &_Error, void *_pProcess, bool _bSuccess)
			{
				fp64 TimeSinceStart;
				{
					DMibLock(m_ExitTimeLock);
					TimeSinceStart = m_TimeSinceStart.f_GetTime();
					if (!_pProcess)
						m_ExitTime = TimeSinceStart;
				}
//				if (mp_LastLaunchOptions.m_bAllowLaunchedInForground && _pProcess)
//					AllowSetForegroundWindow(GetProcessId(_pProcess));
				if (mp_LastLaunchOptions.m_fOnStateChange)
				{
					if (mp_LastLaunchOptions.m_fDispatcher)
					{
						NPtr::TCSharedPointer<CPOSIXLaunchContext> pThis = fg_Explicit(this);
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

			void CPOSIXLaunchContext::fp_OnOutput(EProcessLaunchOutputType _OutputType, NMib::NStr::CStr const &_Output)
			{
				if (mp_LastLaunchOptions.m_fOnOutput)
				{
					if (mp_LastLaunchOptions.m_fDispatcher)
					{
						NPtr::TCSharedPointer<CPOSIXLaunchContext> pThis = fg_Explicit(this);
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

			void CPOSIXLaunchContext::fp_OnExit(uint32 _ExitCode)
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
						NPtr::TCSharedPointer<CPOSIXLaunchContext> pThis = fg_Explicit(this);
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

			bint CPOSIXLaunchContext::f_Open(CProcessLaunchParams const &_Options)
			{
				mp_LastLaunchOptions = _Options;
				return true;
			}
			
			bint CPOSIXLaunchContext::f_Start(EProcessLaunchCloseFlag _Flags)
			{
				if (mp_bStarted)
					DMibError(NStr::CStrNonTracked("Launch has already been started"));
				mp_bStarted = true;
				// Increase ref count for thread
				
				if (!fg_CreatePipe(mp_WakeupPipeRead, mp_WakeupPipeWrite))
					DMibError(NStr::CStrNonTracked(NMib::NPlatform::fg_FormatErrno("pipe (process launch wakeup)", errno)));
					
				fcntl(mp_WakeupPipeRead, F_SETFL, fcntl(mp_WakeupPipeRead, F_GETFL) | O_NONBLOCK);
				
				if (mp_LastLaunchOptions.m_bThreaded)
				{
					f_RefCountIncrease();
					auto CleanupRef = fg_OnScopeExit(
						[&]()
					{
						f_RefCountDecrease();
					}
					);

					this->f_Start(EThreadPriority_High, 0, 0, true);

					CleanupRef.f_Clear();
				}
				else
				{
					{
						DMibLock(mp_NeedTerminationLock);
						mp_NeedTermination = (_Flags & (EProcessLaunchCloseFlag_TerminateProcess | EProcessLaunchCloseFlag_StopProcess));
						if (_Flags & (EProcessLaunchCloseFlag_LingerUntilDone | EProcessLaunchCloseFlag_BlockOnExit))
							mp_bNeedWait = true;
						mp_bClosed = true;
					}
					ch8 Temp = 0;
					write(mp_WakeupPipeWrite, &Temp, sizeof(Temp));
					f_Main();
				}
				return true;
			}
			
			void CPOSIXLaunchContext::f_Close(EProcessLaunchCloseFlag _Flags)
			{
				bool bNeedWait;
				{
					{
						DMibLock(mp_NeedTerminationLock);
						mp_NeedTermination = (_Flags & (EProcessLaunchCloseFlag_TerminateProcess | EProcessLaunchCloseFlag_StopProcess));
						if (_Flags & (EProcessLaunchCloseFlag_LingerUntilDone | EProcessLaunchCloseFlag_BlockOnExit))
							mp_bNeedWait = true;
						mp_bClosed = true;
						bNeedWait = mp_bNeedWait;
					}
					ch8 Temp = 0;
					write(mp_WakeupPipeWrite, &Temp, sizeof(Temp));
				}
				if (_Flags & EProcessLaunchCloseFlag_BlockOnExit || !bNeedWait) // If we are not lingering we need to make sure that the thread is stopped
					f_Stop(true);
			}

			bint CPOSIXLaunchContext::f_IsRunning()
			{
				return (f_GetState() == NThread::EThreadState_Running);
			}
			
			void CPOSIXLaunchContext::f_SendText(NStr::CStr const &_Text)
			{
				if (!mp_hStdinWrite)
					return;
				
				write(mp_hStdinWrite, _Text.f_GetStr(), _Text.f_GetLen());
			}
			
			fp64 CPOSIXLaunchContext::f_GetRunningTime()
			{
				DMibLock(m_ExitTimeLock);
				if (f_IsRunning())
					return m_TimeSinceStart.f_GetTime();
				else
					return m_ExitTime;
			}
			
			mint CPOSIXLaunchContext::f_GetID()
			{
				return mp_ProcessID;
			}
						
			void CPOSIXLaunchContext::f_Cancel()
			{
				{
					DMibLock(mp_NeedTerminationLock);
					mp_bNeedWait = false;
				}
				ch8 Temp = 0;
				write(mp_WakeupPipeWrite, &Temp, sizeof(Temp));
			}
			
			bool CPOSIXLaunchContext::f_OverallStatsAvailable() const
			{
				return mp_bOverallStatsAvailable.f_Load();
			}

			
			namespace
			{
				fp64 fg_TimeValToSeconds(timeval const &_Time)
				{
					return fp64(_Time.tv_sec) + fp64(_Time.tv_usec) * fp64(0.000001);
				}
				
				void fg_ConvertMemoryStatistics(CProcessStatistics &_Dest, rusage const &_Info)
				{
					if (_Info.ru_maxrss)
						_Dest.m_Statistics("Max resident size", CProcessStat(EProcessStatUnit_Bytes, _Info.ru_maxrss, 1024 * 1024));
					
					if (_Info.ru_ixrss)
						_Dest.m_Statistics("Shared text segment", CProcessStat(EProcessStatUnit_Bytes, _Info.ru_ixrss * 1024, 1024 * 1024));
					
					if (_Info.ru_idrss)
						_Dest.m_Statistics("Unshared text segment", CProcessStat(EProcessStatUnit_Bytes, _Info.ru_idrss * 1024, 1024 * 1024));
					
					if (_Info.ru_isrss)
						_Dest.m_Statistics("Unshared stack segment", CProcessStat(EProcessStatUnit_Bytes, _Info.ru_idrss * 1024, 1024 * 1024));
					
					if (_Info.ru_minflt)
						_Dest.m_Statistics("Soft page faults", CProcessStat(EProcessStatUnit_GeneralNumber, _Info.ru_minflt, 1, "faults"));
					
					if (_Info.ru_majflt)
						_Dest.m_Statistics("Hard page faults", CProcessStat(EProcessStatUnit_GeneralNumber, _Info.ru_majflt, 1, "faults"));
					
					if (_Info.ru_nswap)
						_Dest.m_Statistics("Application swapped", CProcessStat(EProcessStatUnit_GeneralNumber, _Info.ru_nswap, 1, "times"));
				}

	#ifdef DPlatformFamily_OSX
				
				void fg_ConvertMemoryStatistics(CProcessStatistics &_Dest, proc_taskinfo const &_Info)
				{
					_Dest.m_Statistics("Virtual size", CProcessStat(EProcessStatUnit_Bytes, _Info.pti_virtual_size, 1024 * 1024));
					_Dest.m_Statistics("Resident size", CProcessStat(EProcessStatUnit_Bytes, _Info.pti_resident_size, 1024 * 1024));

					if (_Info.pti_faults)
						_Dest.m_Statistics("Page faults", CProcessStat(EProcessStatUnit_GeneralNumber, _Info.pti_faults, 1, "faults"));

					if (_Info.pti_pageins)
						_Dest.m_Statistics("Page ins", CProcessStat(EProcessStatUnit_GeneralNumber, _Info.pti_pageins, 1, "faults"));
					
					if (_Info.pti_cow_faults)
						_Dest.m_Statistics("Page faults COW", CProcessStat(EProcessStatUnit_GeneralNumber, _Info.pti_cow_faults, 1, "faults"));
				}
	#endif
				
	#if DPlatformVersionMax <= 1050
	#define pbi_start_tvsec pbi_start.tv_sec
	#define pbi_start_tvusec pbi_start.tv_usec
	#endif

				void fg_ConvertExecutionStatistics
					(
						CProcessStatistics &_Dest
						, rusage const &_Info
	#ifdef DPlatformFamily_OSX
						, proc_taskallinfo const &_ProcInfo
	#endif
					 )
				{
					fp64 RunTime = 0.0;
	#ifdef DPlatformFamily_OSX
					if (_ProcInfo.pbsd.pbi_start_tvsec)
					{
						static NTime::CTime EpochStart = NTime::CTimeConvert::fs_CreateTime(1970, 1, 1);
						int64 Seconds = _ProcInfo.pbsd.pbi_start_tvsec;
						fp64 Fraction = fp64(_ProcInfo.pbsd.pbi_start_tvusec) / fp64(1000000);
						NTime::CTime StartTime = EpochStart + NTime::CTimeSpanConvert_BabylonianCommon::fs_CreateSpan(0, 0, 0, 0, Seconds, Fraction);
						
						RunTime = (NTime::CTime::fs_NowUTC() - StartTime).f_GetSecondsFraction();
					}
	#endif
					
					fp64 UserTime = fg_TimeValToSeconds(_Info.ru_utime);
					fp64 SystemTime = fg_TimeValToSeconds(_Info.ru_stime);

					if (RunTime > 0.0)
					{
						_Dest.m_Statistics("CPU utilization   Total", CProcessStat(EProcessStatUnit_Fraction, (SystemTime + UserTime) / RunTime));
						_Dest.m_Statistics("CPU utilization  User", CProcessStat(EProcessStatUnit_Fraction, UserTime / RunTime));
						_Dest.m_Statistics("CPU utilization Kernel", CProcessStat(EProcessStatUnit_Fraction, SystemTime / RunTime));
					}
					else
					{
						_Dest.m_Statistics("CPU time   Total", CProcessStat(EProcessStatUnit_Seconds, UserTime+SystemTime, 0.001));
						_Dest.m_Statistics("CPU time  User", CProcessStat(EProcessStatUnit_Seconds, UserTime, 0.001));
						_Dest.m_Statistics("CPU time Kernel", CProcessStat(EProcessStatUnit_Seconds, SystemTime, 0.001));
					}
					
					auto ContextSwitches = _Info.ru_nvcsw + _Info.ru_nivcsw;
					if (ContextSwitches)
						_Dest.m_Statistics("Context switches", CProcessStat(EProcessStatUnit_GeneralNumber, ContextSwitches, 1, "times"));

				}

	#ifdef DPlatformFamily_OSX
				void fg_ConvertExecutionStatistics(CProcessStatistics &_Dest, proc_taskallinfo const &_ProcInfo)
				{
					fp64 RunTime = 0.0;
					if (_ProcInfo.pbsd.pbi_start_tvsec)
					{
						static NTime::CTime EpochStart = NTime::CTimeConvert::fs_CreateTime(1970, 1, 1);
						int64 Seconds = _ProcInfo.pbsd.pbi_start_tvsec;
						fp64 Fraction = fp64(_ProcInfo.pbsd.pbi_start_tvusec) / fp64(1000000);
						NTime::CTime StartTime = EpochStart + NTime::CTimeSpanConvert_BabylonianCommon::fs_CreateSpan(0, 0, 0, 0, Seconds, Fraction);
						
						RunTime = (NTime::CTime::fs_NowUTC() - StartTime).f_GetSecondsFraction();
					}
					
					fp64 UserTime = fp64(_ProcInfo.ptinfo.pti_total_user) / fp64(1000000000.0);
					fp64 SystemTime = fp64(_ProcInfo.ptinfo.pti_total_system) / fp64(1000000000.0);
					
					if (RunTime > 0.0)
					{
						_Dest.m_Statistics("CPU utilization   Total", CProcessStat(EProcessStatUnit_Fraction, (SystemTime + UserTime) / RunTime));
						_Dest.m_Statistics("CPU utilization  User", CProcessStat(EProcessStatUnit_Fraction, UserTime / RunTime));
						_Dest.m_Statistics("CPU utilization Kernel", CProcessStat(EProcessStatUnit_Fraction, SystemTime / RunTime));
					}
					else
					{
						_Dest.m_Statistics("CPU time   Total", CProcessStat(EProcessStatUnit_Seconds, UserTime+SystemTime, 0.001));
						_Dest.m_Statistics("CPU time  User", CProcessStat(EProcessStatUnit_Seconds, UserTime, 0.001));
						_Dest.m_Statistics("CPU time Kernel", CProcessStat(EProcessStatUnit_Seconds, SystemTime, 0.001));
					}
					
					auto ContextSwitches = _ProcInfo.ptinfo.pti_csw;
					if (ContextSwitches)
						_Dest.m_Statistics("Context switches", CProcessStat(EProcessStatUnit_GeneralNumber, ContextSwitches, 1, "times"));
				}
	#endif
				
			}
			
			void CPOSIXLaunchContext::fp_UpdateOverallStats
				(
					rusage const &_RUsage
	#ifdef DPlatformFamily_OSX
					, proc_taskallinfo const &_TaskInfo
	#endif
				)
			{
				{
					DMibLock(mp_OverallStatsLock);
					fg_ConvertMemoryStatistics(mp_OverallMemoryStatistics, _RUsage);
	//					fg_ConvertMemoryStatistics(mp_OverallMemoryStatistics, _TaskInfo.ptinfo);
					fg_ConvertExecutionStatistics
						(
							mp_OverallExecutionStatistics
							, _RUsage
	#ifdef DPlatformFamily_OSX
							, _TaskInfo
	#endif
						)
					;
				}
				
				mp_bOverallStatsAvailable.f_Store(1);
			}
		}
	}
}

void *NMib::NProcess::NPlatform::fg_ProcessLaunch_Open(CProcessLaunchParams const &_Params)
{
	NPtr::TCSharedPointer<NPlatform::CPOSIXLaunchContext> pRedir = fg_Construct();

	pRedir->f_RefCountIncrease();

	auto CleanupRef = fg_OnScopeExit(
		[&]()
	{
		pRedir->f_RefCountDecrease();
	}
	);

	pRedir->f_Open(_Params);

	CleanupRef.f_Clear();

	return pRedir.f_Get();
}

void NMib::NProcess::NPlatform::fg_ProcessLaunch_Start(void *_pLaunch, EProcessLaunchCloseFlag _Flags)
{
	NPlatform::CPOSIXLaunchContext *pLaunch = fg_AutoStaticCast(_pLaunch);
	pLaunch->f_Start(_Flags);
}

void NMib::NProcess::NPlatform::fg_ProcessLaunch_Close(void *_pLaunch, EProcessLaunchCloseFlag _Flags)
{
	NPlatform::CPOSIXLaunchContext *pLaunch = fg_AutoStaticCast(_pLaunch);
	pLaunch->f_Close(_Flags);
	if (pLaunch->f_RefCountDecrease() == 0)
		delete pLaunch;
}

bint NMib::NProcess::NPlatform::fg_ProcessLaunch_IsRunning(void *_pLaunch)
{
	NPlatform::CPOSIXLaunchContext *pLaunch = fg_AutoStaticCast(_pLaunch);
	return pLaunch->f_IsRunning();
}
	
void NMib::NProcess::NPlatform::fg_ProcessLaunch_SendStdIn(void *_pLaunch, NMib::NStr::CStr const &_Data)
{
	NPlatform::CPOSIXLaunchContext *pLaunch = fg_AutoStaticCast(_pLaunch);
	pLaunch->f_SendText(_Data);
}

fp64 NMib::NProcess::NPlatform::fg_ProcessLaunch_GetRunningTime(void *_pLaunch)
{
	NPlatform::CPOSIXLaunchContext *pLaunch = fg_AutoStaticCast(_pLaunch);
	return pLaunch->f_GetRunningTime();
}

void NMib::NProcess::NPlatform::fg_ProcessLaunch_Stop(void *_pLaunch)
{
	NPlatform::CPOSIXLaunchContext *pLaunch = fg_AutoStaticCast(_pLaunch);
	NMib::NStr::CStr Error;
	if (!NPlatform::fg_StopProcess(pLaunch->f_GetID(), Error))
		DMibError(Error);
}

mint NMib::NProcess::NPlatform::fg_ProcessLaunch_GetID(void *_pLaunch)
{
	NPlatform::CPOSIXLaunchContext *pLaunch = fg_AutoStaticCast(_pLaunch);
	return pLaunch->f_GetID();
}
			
NMib::NProcess::CProcessStatistics NMib::NProcess::NPlatform::fg_ProcessLaunch_GetExecutionStatistics(void *_pLaunch)
{
	
	CProcessStatistics Stats;

#ifdef DPlatformFamily_OSX
	NPlatform::CPOSIXLaunchContext *pLaunch = fg_AutoStaticCast(_pLaunch);
	int bytes;
	proc_taskallinfo TaskInfoAll = {0};
	bytes = proc_pidinfo(pLaunch->f_GetID(), PROC_PIDTASKINFO, 0, &TaskInfoAll.ptinfo, sizeof(TaskInfoAll.ptinfo));
	bytes = proc_pidinfo(pLaunch->f_GetID(), PROC_PIDTBSDINFO, 0, &TaskInfoAll.pbsd, sizeof(TaskInfoAll.pbsd));
	
	if (bytes <= 0)
	{
		int ErrNo = errno;
		(void)ErrNo;
		//DMibDTrace("proc_pidinfo {}\n", NMib::NPlatform::fg_FormatErrno("", ErrNo));
	}
	else if (bytes < sizeof(TaskInfoAll.pbsd))
	{
		int ErrNo = errno;
		(void)ErrNo;
		//DMibDTrace("proc_pidinfo {} < {} = {}\n", bytes << (sizeof(TaskInfoAll)) << NMib::NPlatform::fg_FormatErrno("", ErrNo));
	}
	else
		fg_ConvertExecutionStatistics(Stats, TaskInfoAll);
#endif
	
	return Stats;
}

NMib::NProcess::CProcessStatistics NMib::NProcess::NPlatform::fg_ProcessLaunch_GetMemoryStatistics(void *_pLaunch)
{

	NMib::NProcess::CProcessStatistics Stats;
	
#ifdef DPlatformFamily_OSX
	NMib::NProcess::NPlatform::CPOSIXLaunchContext *pLaunch = NMib::fg_AutoStaticCast(_pLaunch);
	int bytes;
	proc_taskinfo TaskInfo = {0};
	bytes = proc_pidinfo(pLaunch->f_GetID(), PROC_PIDTASKINFO, 0, &TaskInfo, sizeof(TaskInfo));
	
	if (bytes <= 0)
	{
		int ErrNo = errno;
		(void)ErrNo;
		//DMibDTrace("proc_pidinfo {}\n", NMib::NPlatform::fg_FormatErrno("", ErrNo));
	}
	else if (bytes < sizeof(TaskInfo))
	{
		int ErrNo = errno;
		(void)ErrNo;
		//DMibDTrace("proc_pidinfo {} < {} = {}\n", bytes << (sizeof(TaskInfo)) << NMib::NPlatform::fg_FormatErrno("", ErrNo));
	}
	else
		NMib::NProcess::NPlatform::fg_ConvertMemoryStatistics(Stats, TaskInfo);
#endif

	return Stats;
}

NMib::NProcess::CProcessStatistics NMib::NProcess::NPlatform::fg_ProcessLaunch_GetOverallExecutionStatistics(void *_pLaunch)
{
	NPlatform::CPOSIXLaunchContext *pLaunch = fg_AutoStaticCast(_pLaunch);
	return pLaunch->f_OverallExecutionStatistics();
}

NMib::NProcess::CProcessStatistics NMib::NProcess::NPlatform::fg_ProcessLaunch_GetOverallMemoryStatistics(void *_pLaunch)
{
	NPlatform::CPOSIXLaunchContext *pLaunch = fg_AutoStaticCast(_pLaunch);
	return pLaunch->f_OverallMemoryStatistics();
}


void NMib::NProcess::NPlatform::fg_Process_GetMemoryCurrentStatistics(void *_pProcess, CProcessStatistics &_Stats)
{
#ifdef DPlatformFamily_OSX
	mint ProcessID = (mint)_pProcess;
	int bytes;
	proc_taskinfo TaskInfo = {0};
	bytes = proc_pidinfo(ProcessID, PROC_PIDTASKINFO, 0, &TaskInfo, sizeof(TaskInfo));
	
	if (bytes <= 0)
	{
		int ErrNo = errno;
		(void)ErrNo;
		//DMibDTrace("proc_pidinfo {}\n", NMib::NPlatform::fg_FormatErrno("", ErrNo));
	}
	else if (bytes < sizeof(TaskInfo))
	{
		int ErrNo = errno;
		(void)ErrNo;
		//DMibDTrace("proc_pidinfo {} < {} = {}\n", bytes << (sizeof(TaskInfo)) << NMib::NPlatform::fg_FormatErrno("", ErrNo));
	}
	else
		fg_ConvertMemoryStatistics(_Stats, TaskInfo);
#endif
}

void NMib::NProcess::NPlatform::fg_Process_GetExecutionCurrentStatistics(void *_pProcess, CProcessStatistics &_Stats)
{
#ifdef DPlatformFamily_OSX
	mint ProcessID = (mint)_pProcess;
	int bytes;
	proc_taskallinfo TaskInfoAll = {0};
	bytes = proc_pidinfo(ProcessID, PROC_PIDTASKINFO, 0, &TaskInfoAll.ptinfo, sizeof(TaskInfoAll.ptinfo));
	bytes = proc_pidinfo(ProcessID, PROC_PIDTBSDINFO, 0, &TaskInfoAll.pbsd, sizeof(TaskInfoAll.pbsd));
	
	if (bytes <= 0)
	{
		int ErrNo = errno;
		(void)ErrNo;
		//DMibDTrace("proc_pidinfo {}\n", NMib::NPlatform::fg_FormatErrno("", ErrNo));
	}
	else if (bytes < sizeof(TaskInfoAll.pbsd))
	{
		int ErrNo = errno;
		(void)ErrNo;
		//DMibDTrace("proc_pidinfo {} < {} = {}\n", bytes << (sizeof(TaskInfoAll)) << NMib::NPlatform::fg_FormatErrno("", ErrNo));
	}
	else
		fg_ConvertExecutionStatistics(_Stats, TaskInfoAll);
#endif			
}		


void NMib::NProcess::NPlatform::fg_Process_GetMemoryOverallStatistics(void *_pProcess, CProcessStatistics &_Stats)
{
}

void NMib::NProcess::NPlatform::fg_Process_GetExecutionOverallStatistics(void *_pProcess, CProcessStatistics &_Stats)
{
}

void NMib::NProcess::NPlatform::fg_ProcessLaunch_CancelAll()
{
	auto &SubProcess = *NPlatform::g_SubSystem_Process_Platform_POSIX_Launch;
	bool bDoneCancel = false;
	while (1)
	{
		{
			DMibLock(SubProcess.m_LaunchesLock);
			if (SubProcess.m_Launches.f_IsEmpty())
				return;
			if (!bDoneCancel)
			{
				bDoneCancel = true;
				for (auto Iter = SubProcess.m_Launches.f_GetIterator(); Iter; ++Iter)
					static_cast<NPlatform::CPOSIXLaunchContext *>(Iter.f_GetCurrent())->f_Cancel();
			}
		}
		NMib::NSys::fg_Thread_Sleep(0.001);
	}
}

void NMib::NProcess::NPlatform::fg_Process_RegisterURLHandler(NMib::NStr::CStr const &_Protocol, NMib::NStr::CStr const& _ExePath, NMib::NStr::CStr const &_Params)
{
	#ifdef DPlatformFamily_OSX
		NPlatform::fg_MacOSX_RegisterURLHandler(_Protocol, _ExePath, _Params);
	#elif defined(DPlatformFamily_Linux)
		NPlatform::fg_Linux_RegisterURLHandler(_Protocol, _ExePath, _Params);
	#endif
}

void NMib::NProcess::NPlatform::fg_Process_DeRegisterURLHandler(NMib::NStr::CStr const &_Protocol)
{
	#ifdef DPlatformFamily_OSX
		NPlatform::fg_MacOSX_DeRegisterURLHandler(_Protocol);
	#elif defined(DPlatformFamily_Linux)
		NPlatform::fg_Linux_DeRegisterURLHandler(_Protocol);
	#endif
}

void NMib::NProcess::NPlatform::fg_Process_RegisterAtStartup(NMib::NStr::CStr const& _ExePath, NMib::NStr::CStr const &_Params, NMib::NStr::CStr const& _Name)
{
	#ifdef DPlatformFamily_OSX
		NPlatform::fg_MacOSX_Process_RegisterAtStartup(_ExePath, _Params, _Name);
	#elif defined(DPlatformFamily_Linux)
		NPlatform::fg_Linux_Process_RegisterAtStartup(_ExePath, _Params, _Name);
	#endif
	// DMibPDebugBreak; // Not implemented
}

void NMib::NProcess::NPlatform::fg_Process_DeRegisterAtStartup(NMib::NStr::CStr const& _ExePath, NMib::NStr::CStr const &_Params, NMib::NStr::CStr const& _Name)
{
	#ifdef DPlatformFamily_OSX
		NPlatform::fg_MacOSX_Process_DeRegisterAtStartup(_ExePath, _Params, _Name);
	#elif defined(DPlatformFamily_Linux)
		NPlatform::fg_Linux_Process_DeRegisterAtStartup(_ExePath, _Params, _Name);
	#endif
	// DMibPDebugBreak; // Not implemented
}

NMib::NProcess::EProcessElevation NMib::NProcess::NPlatform::fg_Process_GetElevation()
{		
	if (geteuid() == 0 && getuid() != 0)
		return EProcessElevation_IsElevated;
	else if (geteuid() == 0 && getuid() == 0)
		return EProcessElevation_IsRoot;
	
	return EProcessElevation_IsNotElevated;
}
