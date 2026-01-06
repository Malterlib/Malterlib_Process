// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include "Malterlib_Process_Platform_POSIX_LaunchLimiter.h"
#include "Malterlib_Process_Platform_POSIX.h"

#include "../Malterlib_Process_Platform.h"

namespace NMib::NProcess::NPlatform
{
	struct CSubSystem_Process_Platform_POSIX_LaunchLimiter : public CSubSystem
	{
		NThread::CMutual m_LaunchLimitersLock;
		NContainer::TCMap<NStr::CStr, CProcessLaunchLimiter> m_LaunchLimiters;

		CSharedLimiter f_GetCpuLimiter(NStr::CStr const &_ProcessGroup, fp32 _CPUUsage, pid_t _ProcessID)
		{
			DMibLock(m_LaunchLimitersLock);
			auto Mapped = m_LaunchLimiters(_ProcessGroup, fg_Clamp(_CPUUsage, 0.001, 0.999));
			auto &Ret = Mapped.f_GetResult();

			return fg_Construct(&Ret, _ProcessID);
		}
	};

	constinit TCSubSystem<CSubSystem_Process_Platform_POSIX_LaunchLimiter, ESubSystemDestruction_BeforeMemoryManager> g_SubSystem_Process_Platform = {DAggregateInit};

	CSharedLimiter fg_GetCPULimiter(NStr::CStr const &_ProcessGroup, fp32 _CPUUsage, pid_t _ProcessID)
	{
		return g_SubSystem_Process_Platform->f_GetCpuLimiter(_ProcessGroup, _CPUUsage, _ProcessID);
	}

	CCPULimiterRef::CCPULimiterRef(CProcessLaunchLimiter *_pLimiter, pid_t _ProcessID)
		: m_pLimiter(_pLimiter)
		, m_ProcessID(_ProcessID)
	{
		_pLimiter->f_AddPid(_ProcessID);
	}

	CCPULimiterRef::~CCPULimiterRef()
	{
		auto &SubSystem = *g_SubSystem_Process_Platform;
		DMibLock(SubSystem.m_LaunchLimitersLock);
		if (m_pLimiter->f_RemovePid(m_ProcessID))
		{
			SubSystem.m_LaunchLimiters.f_Remove(m_pLimiter);
		}
	}



	CProcessLaunchLimiter::CProcessEntry::CProcessEntry()
	{
	}

	CProcessLaunchLimiter::CProcessEntry::~CProcessEntry()
	{
		if (m_pPausedToken)
			fg_Process_Resume(f_GetID(), m_pPausedToken);
	}

	void CProcessLaunchLimiter::CProcessEntry::f_MapProcess(pid_t _ID, uint64 _StartTime)
	{
		auto &Mapped = m_AllProcesses[_ID];
		Mapped.m_bTouched = false;
		Mapped.m_StartTime = _StartTime;
	}

	void CProcessLaunchLimiter::CProcessEntry::f_MapProcessParent(pid_t _ID, pid_t _ParentID)
	{
		CProcessEntry &Entry = m_AllProcesses[_ID];

		CProcessEntry *pParent = m_AllProcesses.f_FindEqual(_ParentID);

		if (!pParent)
			return;

		pParent->m_Children.f_Insert(Entry);
	}

	void CProcessLaunchLimiter::CProcessEntry::f_Touch()
	{
		for (auto iProcess = m_AllProcesses.f_GetIterator(); iProcess; ++iProcess)
		{
			iProcess->m_bTouched = true;
		}
	}

	void CProcessLaunchLimiter::CProcessEntry::f_RemoveTouched()
	{
		for (auto iProcess = m_AllProcesses.f_GetIterator(); iProcess; )
		{
			if (iProcess->m_bTouched)
			{
				iProcess.f_Remove();
			}
			else
				++iProcess;
		}
	}

	void CProcessLaunchLimiter::CProcessEntry::fr_PruneByTime(uint64 _Time)
	{
		for (auto iChild = m_Children.f_GetIterator(); iChild; )
		{
			if (iChild->m_StartTime < _Time)
			{
				iChild.f_Remove();
			}
			else
			{
				iChild->fr_PruneByTime(_Time);
				++iChild;
			}
		}
	}

	void CProcessLaunchLimiter::CProcessEntry::f_PruneByTime()
	{
		fr_PruneByTime(m_StartTime);
	}

	void CProcessLaunchLimiter::CProcessEntry::f_EnumChildren(NContainer::TCVector<CProcessEntry *> &_oChildren)
	{
		_oChildren.f_Insert(this);
		for (auto iChild = m_Children.f_GetIterator(); iChild; ++iChild)
			iChild->f_EnumChildren(_oChildren);
	}

	bool CProcessLaunchLimiter::CProcessEntry::f_PauseTree(NContainer::TCVector<CProcessEntry *> const &_Children)
	{
		mint nChildren = _Children.f_GetLen();
		mint nLoops = nChildren;
		++m_StartPause;
		if (m_StartPause >= nChildren)
			m_StartPause = 0;

		bool bRet = false;
		for (mint iChild = m_StartPause; nLoops; --nLoops)
		{
			auto &Child = *(_Children[iChild]);
			if (!Child.m_pPausedToken)
			{
				Child.m_pPausedToken = fg_Process_Pause(Child.f_GetID());
				bRet = true;
			}
			++iChild;
			if (iChild >= nChildren)
				iChild = 0;
		}

		return bRet;
	}

	bool CProcessLaunchLimiter::CProcessEntry::f_UnPauseTree(NContainer::TCVector<CProcessEntry *> const &_Children)
	{
		mint nChildren = _Children.f_GetLen();
		mint nLoops = nChildren;
		if (m_StartPause >= nChildren)
			m_StartPause = 0;

		bool bRet = false;
		for (mint iChild = m_StartPause; nLoops; --nLoops)
		{
			auto &Child = *(_Children[iChild]);
			if (Child.m_pPausedToken)
			{
				fg_Process_Resume(Child.f_GetID(), Child.m_pPausedToken);
				Child.m_pPausedToken = nullptr;
				bRet = true;
			}
			++iChild;
			if (iChild >= nChildren)
				iChild = 0;
		}

		return bRet;
	}

	fp32 CProcessLaunchLimiter::f_Update()
	{
		fp64 Now = m_Clock.f_GetTime();
		fp64 LastUpdateTime = Now - m_LastUpdate;
		m_LastUpdate = Now;
		auto Processes = fg_Posix_ImpSpecefic_EnumProcesses();

		NContainer::TCSet<pid_t> ProcessIDs;
		{
			DMibLock(m_ProcessIDsLock);
			ProcessIDs = m_ProcessIDs;
		}

		m_RootEntry.f_Touch();

		for (auto iProcess = Processes.f_GetIterator(); iProcess; ++iProcess)
			m_RootEntry.f_MapProcess(iProcess->m_ProcessID, iProcess->m_StartTime);

		m_RootEntry.f_RemoveTouched();

		for (auto iProcess = Processes.f_GetIterator(); iProcess; ++iProcess)
			m_RootEntry.f_MapProcessParent(iProcess->m_ProcessID, iProcess->m_ParentProcessID);

		NContainer::TCVector<CProcessEntry *> Children;
		for (auto iProcess = ProcessIDs.f_GetIterator(); iProcess; ++iProcess)
		{
			auto pProcess = m_RootEntry.m_AllProcesses.f_FindEqual(*iProcess);
			if (pProcess)
			{
				pProcess->f_PruneByTime();
				pProcess->f_EnumChildren(Children);
			}
		}
		fp64 NextUpdate;
		fp64 Interval = 1.0 / 240.0;

		if (m_bDidPause)
		{
			m_RootEntry.f_UnPauseTree(Children);
			NextUpdate = -(m_CPULimit / (fp64(fp64(1.0)/Interval) * m_CPULimit - fp64(fp64(1.0)/Interval)));
			//DMibTrace("Unpausing for: {} {}\n", NextUpdate << LastUpdateTime);
		}
		else
		{
			m_RootEntry.f_PauseTree(Children);
			//fp64 ReverseLimit = (1.0 - m_CPULimit.f_Get());
			//NextUpdate = -(ReverseLimit / (fp64(10.0) * ReverseLimit - fp64(10.0)));
			fp64 WantedTime = NextUpdate = -(m_CPULimit / (fp64(fp64(1.0)/Interval) * m_CPULimit - fp64(fp64(1.0)/Interval)));
			NextUpdate = Interval * (LastUpdateTime / WantedTime);
			//DMibTrace("Pausing for: {} {}\n", NextUpdate << LastUpdateTime);
		}

		m_bDidPause = !m_bDidPause;

		return NextUpdate;
	}

	void CProcessLaunchLimiter::f_AddPid(pid_t _Pid)
	{
		//DMibConErrOut("f_AddPid: {}\n", _Pid);
		DMibLock(m_ProcessIDsLock);
		m_ProcessIDs[_Pid];
	}

	bool CProcessLaunchLimiter::f_RemovePid(pid_t _Pid)
	{
		//DMibConErrOut("f_RemovePid: {}\n", _Pid);
		DMibLock(m_ProcessIDsLock);
		m_ProcessIDs.f_Remove(_Pid);
		return m_ProcessIDs.f_IsEmpty();
	}

	CProcessLaunchLimiter::CProcessLaunchLimiter(fp32 _CPULimit)
		: m_CPULimit(_CPULimit)
	{
		//fg_AcquireTaskportRight();
		m_pUpdateThread
			= NThread::CThreadObject::fs_StartThread
			(
				[this](NThread::CThreadObject *_pThread) -> aint
				{
					m_Clock.f_Start();
					while (_pThread->f_GetState() != NThread::EThreadState_EventWantQuit)
					{
						fp32 NextWakeup = f_Update();
						_pThread->m_EventWantQuit.f_WaitTimeout(NextWakeup);
					}
					return 0;
				}
				, "CProcessLaunchLimiter"
				, EExecutionPriority_Highest
			)
		;
	}

	CProcessLaunchLimiter::~CProcessLaunchLimiter()
	{
		m_pUpdateThread.f_Clear();
	}
}
