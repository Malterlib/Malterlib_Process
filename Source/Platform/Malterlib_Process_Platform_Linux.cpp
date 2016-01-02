// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Core/PlatformSpecific/PosixErrNo>
#include <Mib/Core/PlatformSpecific/LinuxProcFS>

#include "../Malterlib_Process_Platform.h"

#include "Malterlib_Process_Platform_POSIX.h"
#include "Malterlib_Process_Platform_POSIX_PlatformSpecific.h"
#include "Malterlib_Process_Platform_POSIX_PlatformSpecific.h"

#include <signal.h>
#include <errno.h>

bool NMib::NProcess::NPlatform::fg_Process_IsRunning(mint _ProcessID)
{
	try
	{
		NMib::NStr::CStrNonTracked FileName = NMib::NStr::CStrNonTracked::CFormat("/proc/{}/stat") << _ProcessID;

		if (!NMib::NFile::CFile::fs_FileExists(FileName))
			return false;

		auto FileData = NMib::NPlatform::fg_ReadProcFSNonTracked(FileName);
		
		NMib::NStr::CStrPtr Data;
		Data.f_SetConstPtr(FileData.f_GetArray(), FileData.f_GetLen());
		
		aint nParsed;
		int32 pid = 0;
		NMib::NStr::CStr comm;
		NMib::NStr::CStr state;
		
		(
			NMib::NStr::CStrPtr::CParse("{} ({}) {} ") 
			>> pid 
			>> comm 
			>> state 
		).f_Parse(Data, nParsed);
		
		if (nParsed != 3)
			return false;
		
		if (state == "Z")
			return false;
		return true;
	}
	catch (NException::CException const &)
	{
	}
	
	return false;
}


NMib::NStr::CStr NMib::NProcess::NPlatform::fg_Process_GetOperatingSystemTag(int32 _MajorMax, int32 _MinorMax)
{
	int Major, Minor, Fix;
	EOperatingSystemArch Arch;
	NSys::fg_System_GetOperatingSystemVersion(Major, Minor, Fix, Arch);
	
	if (Major > _MajorMax || (Major == _MajorMax && Minor > _MinorMax))
	{
		Major = _MajorMax;
		Minor = _MinorMax;
	}

	return (NStr::CStr::CFormat("Linux{}.{}") << Major << Minor).f_GetStr();
}

NMib::NStr::CStr NMib::NProcess::NPlatform::fg_Process_GetOperatingSystemDescription()
{
	int Major, Minor, Fix;
	EOperatingSystemArch Arch;
	NSys::fg_System_GetOperatingSystemVersion(Major, Minor, Fix, Arch);

	return (NStr::CStr::CFormat("Linux {}.{}") << Major << Minor).f_GetStr();
}

uint64 NMib::NProcess::NPlatform::fg_Process_GetPhysicalMemory()
{
    return uint64(sysconf(_SC_PHYS_PAGES)) * uint64(sysconf(_SC_PAGE_SIZE));
}

namespace
{

	struct CProcInfo
	{
		int32 m_ProcessID;
		int32 m_ParentProcessID;
		uint64 m_StartTime;
	};
	NMib::NContainer::TCVector<CProcInfo> fg_Linux_Process_GetAllRunning()
	{
		NMib::NContainer::TCVector<CProcInfo> Return;
		
		auto Files = NMib::NFile::CFile::fs_FindFiles("/proc/*", NMib::NFile::EFileAttrib_Directory, false, false);
		
		for (auto iFile = Files.f_GetIterator(); iFile; ++iFile)
		{
			int32 ProcessID = NMib::NStr::CStr(NMib::NFile::CFile::fs_GetFile(*iFile)).f_ToIntExact(int32(-1));
			if (ProcessID >= 0)
			{
				try
				{
					auto FileData = NMib::NPlatform::fg_ReadProcFS(NMib::NStr::CFStr256::CFormat("/proc/{}/stat") << ProcessID);

					NMib::NStr::CStrPtr Data;
					Data.f_SetConstPtr(FileData.f_GetArray(), FileData.f_GetLen());
					
					aint nParsed;
					int32 pid = 0;
					NMib::NStr::CStr comm;
					NMib::NStr::CStr state;
					int32 ppid = 0;
					int32 pgrp = 0;
					int32 session = 0;
					int32 tty_nr = 0;
					int32 tpgid = 0;
					uint32 flags = 0;
					uint64 minflt = 0;
					uint64 cminflt = 0;
					uint64 majflt = 0;
					uint64 cmajflt = 0;
					uint64 utime = 0;
					uint64 stime = 0;
					int64 cutime = 0;
					int64 cstime = 0;
					int64 priority = 0;
					int64 nice = 0;
					int64 num_threads = 0;
					int64 itrealvalue = 0;
					uint64 starttime = 0;
					
					(
						NMib::NStr::CStrPtr::CParse("{} ({}) {} {} {} {} {} {} {} {} {} {} {} {} {} {} {} {} {} {} {} {} ") 
						>> pid 
						>> comm 
						>> state 
						>> ppid
						>> pgrp
						>> session
						>> tty_nr
						>> tpgid
						>> flags
						>> minflt
						>> cminflt
						>> majflt
						>> cmajflt
						>> utime
						>> stime
						>> cutime
						>> cstime
						>> priority
						>> nice
						>> num_threads
						>> itrealvalue
						>> starttime
					).f_Parse(Data, nParsed);
					
					if (nParsed == 22)
					{
						auto &New = Return.f_Insert();
						New.m_ProcessID = pid;
						New.m_ParentProcessID = ppid;
						New.m_StartTime = starttime;						
					}						
					
				}
				catch (NMib::NException::CException const &_Exception)
				{
				}
			}
		}
		
		return Return;			
	}

	struct CProcessEntry
	{
		DMibListLinkDS_Link(CProcessEntry, m_Link);
		DMibListLinkDS_List(CProcessEntry, m_Link) m_Children;
		
		NMib::NContainer::TCMap<pid_t, CProcessEntry> m_AllProcesses;
		
		zbool m_bPaused;
		
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
		if (m_bPaused)
			NMib::NProcess::NPlatform::fg_Process_Resume(f_GetID());
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
		if (!m_bPaused)
		{
			m_bPaused = true;
			bRet = true;
			NMib::NProcess::NPlatform::fg_Process_Pause(f_GetID());
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
		if (kill(f_GetID(), SIGKILL))
		{
			int ErrNo = errno;
			_Log += NMib::NPlatform::fg_FormatErrno(NMib::NStr::CStr::CFormat("kill({}) when killing processes recursively") << f_GetID(), ErrNo);
			_Log += DMibNewLine;
		}
		for (auto iChild = m_Children.f_GetIterator(); iChild; ++iChild)
			iChild->f_KillTree(_Log, _Depth + 1);
	}
}

bool NMib::NProcess::NPlatform::fg_Linux_Process_TerminateTree(pid_t _ProcessID, NStr::CStr &_Errors)
{
	CProcessEntry Root;
	while (true)
	{
		uint64 ThisStartTime;
		bool bFoundThis = false;
		auto RunningProcesses = fg_Linux_Process_GetAllRunning();
		for (auto iProcess = RunningProcesses.f_GetIterator(); iProcess; ++iProcess)
		{
			Root.f_MapProcess(iProcess->m_ProcessID);
			if (iProcess->m_ProcessID == _ProcessID)
			{
				ThisStartTime = iProcess->m_StartTime;
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
			if (iProcess->m_StartTime >= ThisStartTime)
			{
				// Only consider processes that has started after our current process to not catch orphaned processes
				Root.f_MapProcessParent(iProcess->m_ProcessID, iProcess->m_ParentProcessID);
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
	NMib::NContainer::TCVector<CPOSIXProcessInfo> Ret;
	auto Processes = fg_Linux_Process_GetAllRunning();
	for (auto iProcess = Processes.f_GetIterator(); iProcess; ++iProcess)
	{
		auto &Process = *iProcess;
		auto &New = Ret.f_Insert();
		New.m_ProcessID = Process.m_ProcessID;
		New.m_ParentProcessID = Process.m_ParentProcessID;
		New.m_StartTime = Process.m_StartTime;
	}
	return Ret;
}

NMib::NContainer::TCVector<NMib::NProcess::CProcessInfo> NMib::NProcess::NPlatform::fg_Process_Enum(NProcess::EProcessInfoFlag _ToGet, NContainer::TCVector<NProcess::CProcessInfo> * _pOldEnum)
{
	NContainer::TCVector<NProcess::CProcessInfo> Ret;
	auto Processes = fg_Linux_Process_GetAllRunning();
	for (auto iProcess = Processes.f_GetIterator(); iProcess; ++iProcess)
	{
		auto &Process = *iProcess;
		auto &New = Ret.f_Insert();
		New.m_ProcessID = Process.m_ProcessID;
		New.m_ParentProcessID = Process.m_ParentProcessID;
		New.m_StartTime = Process.m_StartTime;
	}
	return Ret;
}		
