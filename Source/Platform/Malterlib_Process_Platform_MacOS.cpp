// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Core/PlatformSpecific/PosixErrNo>

#include "../Malterlib_Process_Platform.h"

#include <sys/types.h>
#include <sys/sysctl.h>
#include <errno.h> 
#include <signal.h>
#include <mach/mach.h>

#include "Malterlib_Process_Platform_POSIX.h"
#include "Malterlib_Process_Platform_POSIX_PlatformSpecific.h"

uint64 NMib::NProcess::NPlatform::fg_Process_GetPhysicalMemory()
{
	int lVars[2];
	uint64 PhysicalMemory = 0;
	size_t Length;

	lVars[0] = CTL_HW;
	lVars[1] = HW_MEMSIZE;
	Length = sizeof(uint64);
	int Ret = sysctl(lVars, 2, &PhysicalMemory, &Length, NULL, 0); // ! This looks portable to Linux but is not!! Should use the /proc/sys route there.

	if (Ret != 0)
	{
		// Throw?
		return 0;
	}

	return PhysicalMemory;
}

namespace
{
	NMib::NContainer::TCVector<kinfo_proc> fg_MacOS_Process_GetAllRunning()
	{
		NMib::NContainer::TCVector<kinfo_proc> Return;
		
		int SysCtlData[4];
		
		SysCtlData[0] = CTL_KERN;
		SysCtlData[1] = KERN_PROC;
		SysCtlData[2] = KERN_PROC_ALL;
		SysCtlData[3] = 0;
		
		while (true) 
		{
			size_t Length = 0;
			int Error = sysctl
				(
					SysCtlData
					, (sizeof(SysCtlData) / sizeof(*SysCtlData)) - 1
					, NULL
					, &Length
					, NULL
					, 0
				)
			;
			if (Error == -1) 
				Error = errno;
			
			if (Error == 0) 
				Return.f_SetLen(Length / sizeof(kinfo_proc) * 2); // Alloc the current size we need times 2 if new process has already been created
			else
				DMibError(NMib::NPlatform::fg_FormatErrno("sysctl (get all processes)", Error));
			
			Length = Return.f_GetLen() * sizeof(kinfo_proc);
			Error = sysctl
				(
					SysCtlData
					, (sizeof(SysCtlData) / sizeof(*SysCtlData)) - 1
					, Return.f_GetArray()
					, &Length
					, NULL
					, 0
				)
			;
			
			if (Error == -1) 
				Error = errno;
			if (Error == 0) 
			{
				Return.f_SetLen(Length / sizeof(kinfo_proc));
				break;
			} 
			else if (Error != ENOMEM) 
				DMibError(NMib::NPlatform::fg_FormatErrno("sysctl (get all processes)", Error));
		}
		
		return Return;			
	}
	struct CProcessEntry
	{
		DMibListLinkDS_Link(CProcessEntry, m_Link);
		DMibListLinkDS_List(CProcessEntry, m_Link) m_Children;
		
		NMib::NContainer::TCMap<pid_t, CProcessEntry> m_AllProcesses;
		
		void *m_pPausedToken = nullptr;
		bool m_bTriedPause = false;

		pid_t f_GetID() const
		{
			return NMib::NContainer::TCMap<pid_t, CProcessEntry>::fs_GetKey(this);
		}
		
		void f_MapProcess(pid_t _ID);
		void f_MapProcessParent(pid_t _ID, pid_t _ParentID);
		void f_KillTree(NMib::NStr::CStr &_Log, aint _Depth = 0);
		bool f_PauseTree();
		~CProcessEntry();
	};


	CProcessEntry::~CProcessEntry()
	{
		if (m_pPausedToken)
			NMib::NProcess::NPlatform::fg_Process_Resume(f_GetID(), m_pPausedToken);
	}

	void CProcessEntry::f_MapProcess(pid_t _ID)
	{
		m_AllProcesses[_ID];
	}

	void CProcessEntry::f_MapProcessParent(pid_t _ID, pid_t _ParentID)
	{
		CProcessEntry &Entry = m_AllProcesses[_ID];
		
		CProcessEntry *pParent = m_AllProcesses.f_FindEqual(_ParentID);
		
		if (!pParent)
			return;

		pParent->m_Children.f_Insert(Entry);
	}

	bool CProcessEntry::f_PauseTree()
	{
		bool bRet = false;
		if (!m_pPausedToken && !m_bTriedPause)
		{
			m_pPausedToken = NMib::NProcess::NPlatform::fg_Process_Pause(f_GetID());
			m_bTriedPause = true;
			bRet = true;
		}
		for (auto iChild = m_Children.f_GetIterator(); iChild; ++iChild)
		{
			if (iChild->f_PauseTree())
				bRet = true;
		}
		return bRet;
	}

	void CProcessEntry::f_KillTree(NMib::NStr::CStr &_Log, aint _Depth)
	{
	//			DMibConOut("{sj*}Killing: {}\n", "" << _Depth*3 << f_GetID());
		if (kill(f_GetID(), SIGKILL))
		{
			int ErrNo = errno;
			_Log += NMib::NPlatform::fg_FormatErrno(NMib::NStr::CStr::CFormat("kill({}, SIGKILL) when terminating processes recursively") << f_GetID(), ErrNo);
			_Log += DMibNewLine;
		}
		for (auto iChild = m_Children.f_GetIterator(); iChild; ++iChild)
			iChild->f_KillTree(_Log, _Depth + 1);
	}
}

bool NMib::NProcess::NPlatform::fg_MacOS_Process_TerminateTree(pid_t _ProcessID, NStr::CStr &_Errors)
{
	CProcessEntry Root;
	while (true)
	{
		timeval ThisStartTime;
		bool bFoundThis = false;
		auto RunningProcesses = fg_MacOS_Process_GetAllRunning();
		for (auto iProcess = RunningProcesses.f_GetIterator(); iProcess; ++iProcess)
		{
			Root.f_MapProcess(iProcess->kp_proc.p_pid);
			if (iProcess->kp_proc.p_pid == _ProcessID)
			{
				ThisStartTime = iProcess->kp_proc.p_starttime;
				bFoundThis = true;
			}
		}
		if (!bFoundThis)
		{
			_Errors += "Could not find process to terminate in process list";
			return false;
		}
		for (auto iProcess = RunningProcesses.f_GetIterator(); iProcess; ++iProcess)
		{
			if 
				(
					iProcess->kp_proc.p_starttime.tv_sec > ThisStartTime.tv_sec
					|| (iProcess->kp_proc.p_starttime.tv_sec == ThisStartTime.tv_sec && iProcess->kp_proc.p_starttime.tv_usec >= ThisStartTime.tv_usec)
				)
			{
				// Only consider processes that has started after our current process to not catch orphaned processes
				Root.f_MapProcessParent(iProcess->kp_proc.p_pid, iProcess->kp_eproc.e_ppid);
			}
		}
		auto pProcess = Root.m_AllProcesses.f_FindEqual(_ProcessID);
		DMibFastCheck(pProcess);
		if (pProcess)
		{
			if (!pProcess->f_PauseTree())
			{
				// This means that no more processes were found this round, so we can kill the tree safely
				pProcess->f_KillTree(_Errors, 0);
				break;
			}
		}
	}
	
	return true;
}

NMib::NContainer::TCVector<NMib::NProcess::NPlatform::CPOSIXProcessInfo> NMib::NProcess::NPlatform::fg_Posix_ImpSpecefic_EnumProcesses()
{
	NContainer::TCVector<NMib::NProcess::NPlatform::CPOSIXProcessInfo> Ret;
	auto Processes = fg_MacOS_Process_GetAllRunning();
	for (auto iProcess = Processes.f_GetIterator(); iProcess; ++iProcess)
	{
		auto &Process = *iProcess;
		auto &New = Ret.f_Insert();
		New.m_ProcessID = Process.kp_proc.p_pid;
		New.m_ParentProcessID = Process.kp_eproc.e_ppid;
		New.m_StartTime = uint64(Process.kp_proc.p_starttime.tv_sec) << 32 | uint64(Process.kp_proc.p_starttime.tv_usec);
	}
	
	return Ret;
}

NMib::NContainer::TCVector<NMib::NProcess::CProcessInfo> NMib::NProcess::NPlatform::fg_Process_Enum(EProcessInfoFlag _ToGet, NContainer::TCVector<CProcessInfo> * _pOldEnum)
{
	int mib[3] = {CTL_KERN, KERN_ARGMAX, 0};
	size_t ArgMaxSize = sizeof(size_t);
	size_t KernMax = 0;
	if (sysctl(mib, 2, &KernMax, &ArgMaxSize, nullptr, 0))
		DMibError(NMib::NPlatform::fg_FormatErrno("sysctl (KERN_ARGMAX)", errno));

	
	NContainer::TCMap<mint, NProcess::CProcessInfo *> OldInfo;
	
	if (_pOldEnum)
	{
		for (auto iOld = _pOldEnum->f_GetIterator(); iOld; ++iOld)
		{
			OldInfo[iOld->m_ProcessID] = &*iOld;
		}
	}
	
	NContainer::CByteVector Data;
	
	NContainer::TCVector<NProcess::CProcessInfo> Ret;
	auto Processes = fg_MacOS_Process_GetAllRunning();
	for (auto iProcess = Processes.f_GetIterator(); iProcess; ++iProcess)
	{
		auto &Process = *iProcess;
		uint64 StartTime = uint64(Process.kp_proc.p_starttime.tv_sec) << 32 | uint64(Process.kp_proc.p_starttime.tv_usec);
		if (auto pOld = OldInfo.f_FindEqual(Process.kp_proc.p_pid))
		{
			if ((*pOld)->m_StartTime == StartTime)
			{
				Ret.f_Insert(fg_Move(*pOld));
				continue;
			}
		}
		auto &New = Ret.f_Insert();
		New.m_ProcessID = Process.kp_proc.p_pid;
		if (_ToGet & NProcess::EProcessInfoFlag_ParentProcessID)
			New.m_ParentProcessID = Process.kp_eproc.e_ppid;
		//if (_ToGet & NProcess::EProcessInfoFlag_StartTime) // Always set start time so old list can be compared against it
		New.m_StartTime = StartTime;
		if (_ToGet & NProcess::EProcessInfoFlag_FileName)
			New.m_FileName.f_AddStr(Process.kp_proc.p_comm);

		if (_ToGet & NProcess::EProcessInfoFlag_User)
		{
			New.m_RealUID = NStr::CStr::fs_ToStr(Process.kp_eproc.e_pcred.p_ruid);
			New.m_RealGID = NStr::CStr::fs_ToStr(Process.kp_eproc.e_pcred.p_rgid);
			New.m_EffectiveUID = NStr::CStr::fs_ToStr(Process.kp_eproc.e_ucred.cr_uid);
			New.m_EffectiveGID = NStr::CStr::fs_ToStr(Process.kp_eproc.e_ucred.cr_groups[0]);
		}

		if ((_ToGet & NProcess::EProcessInfoFlag_FullPath) || (_ToGet & NProcess::EProcessInfoFlag_Args))
		{
			mib[1] = KERN_PROCARGS2;
			mib[2] = (int)Process.kp_proc.p_pid;
			
			Data.f_SetAtLeastLen(KernMax);
			
			size_t Size = KernMax;
			if (!sysctl(mib, 3, Data.f_GetArray(), &Size, NULL, 0))
			{
				// Format of KERN_PROCARGS2 data
				// argc (int32)
				// cmd name (ascii string)
				// padding (1 - 3 null bytes)
				// next command name and padding bytes ....
				// env variable (ascii string)
				// padding (1 -3 null bytes)
				// next env variable
				
				try
				{
					NStream::CBinaryStreamMemoryPtr<NStream::CBinaryStreamNativeEndian> Stream;
					Stream.f_OpenRead(Data.f_GetArray(), Data.f_GetLen());
					
					int32 nArgc;
					Stream >> nArgc;
					NStr::CStr CmdName;
					while (true)
					{
						uint8 Data;
						Stream.f_ConsumeBytes(&Data, 1);
						
						if (!Data)
							break;

						CmdName.f_AddChar(Data);
					}
					
					//NStream::fg_AlignStream(Stream, 4);

					if (_ToGet & NProcess::EProcessInfoFlag_FullPath)
						New.m_FullPath = CmdName;
					
					if (_ToGet & NProcess::EProcessInfoFlag_Args)
					{
						while (nArgc)
						{
							NStr::CStr Argument;
							bool bFirst = true;
							while (!Stream.f_IsAtEndOfStream())
							{
								uint8 Data;
								Stream.f_ConsumeBytes(&Data, 1);
								
								if (!Data)
								{
									if (!bFirst)
										break;
									while (!Data && !Stream.f_IsAtEndOfStream())
										Stream.f_ConsumeBytes(&Data, 1);
								}
								
								bFirst = false;
								Argument.f_AddChar(Data);
							}
							
							//NStream::fg_AlignStream(Stream, 4);
							
							New.m_Args.f_Insert(fg_Move(Argument));
						
							--nArgc;
						}
					}
				}
				catch (NException::CException const&)
				{
					// Protect against broken data
				}
			}
			else if (_ToGet & NProcess::EProcessInfoFlag_FullPath)
			{
				mib[1] = KERN_PROCARGS;
				mib[2] = (int)Process.kp_proc.p_pid;
				Size = KernMax;
				if (!sysctl(mib, 3, Data.f_GetArray(), &Size, NULL, 0))
					New.m_FullPath = NStr::CStr((ch8 const*)Data.f_GetArray());
			}
		}
	}
	
	return Ret;
}		

void *NMib::NProcess::NPlatform::fg_Process_Pause(mint _ProcessID)
{
	if (fg_Process_GetElevation() >= EProcessElevation_IsElevated)
	{
		if (CSystem::ms_PlatformVersion >= 10'09'00)
		{
			mach_port_t Task;
			if (task_for_pid(mach_task_self(), _ProcessID, &Task) != KERN_SUCCESS)
				return nullptr;

			task_suspension_token_t SuspensionToken;
			if (task_suspend2(Task, &SuspensionToken) != KERN_SUCCESS)
				return nullptr;

			return (void *)(mint)SuspensionToken;
		}
		else
		{
			mach_port_t Task;
			if (task_for_pid(mach_task_self(), _ProcessID, &Task) != KERN_SUCCESS)
				return nullptr;

			if (task_suspend(Task) != KERN_SUCCESS)
				return nullptr;

			return (void *)1;
		}
	}
	else
	{
		if (kill(_ProcessID, SIGSTOP))
			return nullptr;

		return (void *)1;
	}
}

void NMib::NProcess::NPlatform::fg_Process_Resume(mint _ProcessID, void *_pPauseToken)
{
	if (!_pPauseToken)
		return;

	if (fg_Process_GetElevation() >= EProcessElevation_IsElevated)
	{
		if (CSystem::ms_PlatformVersion >= 10'09'00)
		{
			task_suspension_token_t SuspensionToken = (mint)_pPauseToken;
			if (task_resume2(SuspensionToken) != KERN_SUCCESS)
				return;
		}
		else
		{
			mach_port_t Task;
			if (task_for_pid(mach_task_self(), _ProcessID, &Task) != KERN_SUCCESS)
				return;

			if (task_resume(Task) != KERN_SUCCESS)
				return;
		}
	}
	else
		kill(_ProcessID, SIGCONT);
}

NMib::NStr::CStr NMib::NProcess::NPlatform::fg_Process_GetOperatingSystemTag(int32 _MajorMax, int32 _MinorMax)
{
	int Major, Minor, Fix;
	NMib::EOperatingSystemArch Arch;
	NMib::NSys::fg_System_GetOperatingSystemVersion(Major, Minor, Fix, Arch);

	if (Major > _MajorMax || (Major == _MajorMax && Minor > _MinorMax))
	{
		Major = _MajorMax;
		Minor = _MinorMax;
	}

	return (NStr::CStr::CFormat("macOS{}.{}") << Major << Minor).f_GetStr();
}

NMib::NStr::CStr NMib::NProcess::NPlatform::fg_Process_GetOperatingSystemDescription()
{
	int Major, Minor, Fix;
	NMib::EOperatingSystemArch Arch;
	NMib::NSys::fg_System_GetOperatingSystemVersion(Major, Minor, Fix, Arch);

	return (NStr::CStr::CFormat("macOS {}.{}") << Major << Minor).f_GetStr();
}

mint NMib::NProcess::NPlatform::fg_Process_GetMaxFilesPerProc()
{
	int SysCtl[2];
	SysCtl[0] = CTL_KERN;
	SysCtl[1] = KERN_MAXFILESPERPROC;
	size_t Size = sizeof(int);
	int MaxFilesPerProc = 0;
	sysctl(SysCtl, sizeof(SysCtl) / sizeof(*SysCtl), (void *)&MaxFilesPerProc, &Size, NULL, 0);

	Size = sizeof(int);
	SysCtl[1] = KERN_MAXFILES;
	int MaxFiles = 0;
	sysctl(SysCtl, sizeof(SysCtl) / sizeof(*SysCtl), (void *)&MaxFiles, &Size, NULL, 0);

	return fg_Min(MaxFilesPerProc, MaxFiles);
}
