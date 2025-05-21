// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>

namespace NMib::NProcess::NPlatform
{
	struct CProcessLaunchLimiter
	{
		NStorage::CIntrusiveRefCount m_RefCount;

		NThread::CMutual m_ProcessIDsLock;
		NContainer::TCSet<pid_t> m_ProcessIDs;
		fp32 m_CPULimit;
		bool m_bDidPause = false;

		NTime::CClock m_Clock;
		zfp64 m_LastUpdate;

		NStorage::TCUniquePointer<NThread::CThreadObject> m_pUpdateThread;

		struct CProcessEntry
		{
			DMibListLinkDS_Link(CProcessEntry, m_Link);
			DMibListLinkDS_List(CProcessEntry, m_Link) m_Children;

			NContainer::TCMap<pid_t, CProcessEntry> m_AllProcesses;

			zuint64 m_StartTime;
			zmint m_StartPause;

			void *m_pPausedToken = nullptr;
			bool m_bTouched = false;
			bool m_bTriedTask = false;

			pid_t f_GetID() const
			{
				return NContainer::TCMap<pid_t, CProcessEntry>::fs_GetKey(this);
			}

			void fr_PruneByTime(uint64 _Time);
			void f_PruneByTime();
			void f_MapProcess(pid_t _ID, uint64 _StartTime);
			void f_MapProcessParent(pid_t _ID, pid_t _ParentID);
			void f_EnumChildren(NContainer::TCVector<CProcessEntry *> &_oChildren);
			bool f_PauseTree(NContainer::TCVector<CProcessEntry *> const &_Children);
			bool f_UnPauseTree(NContainer::TCVector<CProcessEntry *> const &_Children);
			void f_Touch();
			void f_RemoveTouched();
			CProcessEntry();
			~CProcessEntry();
		};

		CProcessEntry m_RootEntry;

		CProcessLaunchLimiter(fp32 _CPULimit);
		~CProcessLaunchLimiter();

		fp32 f_Update();

		void f_AddPid(pid_t _Pid);
		bool f_RemovePid(pid_t _Pid);
	};

	struct CCPULimiterRef
	{
		CProcessLaunchLimiter *m_pLimiter;
		pid_t m_ProcessID;
		CCPULimiterRef(CProcessLaunchLimiter *_pLimiter, pid_t _ProcessID);
		~CCPULimiterRef();
	};

	using CSharedLimiter = NStorage::TCSharedPointer<CCPULimiterRef>;

	CSharedLimiter fg_GetCPULimiter(NStr::CStr const &_ProcessGroup, fp32 _CPUUsage, pid_t _ProcessID);

}
