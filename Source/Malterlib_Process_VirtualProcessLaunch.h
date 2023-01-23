// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>

#include "Malterlib_Process_ProcessLaunch.h"

namespace NMib::NProcess
{
	class CVirtualProcessLaunch
	{
	public:
		virtual ~CVirtualProcessLaunch();

		virtual EProcessLaunchCloseFlag f_GetCloseFlags() const = 0;
		virtual void f_Close(EProcessLaunchCloseFlag _CloseFlags) = 0;
		virtual void f_StopProcess() const = 0;
		virtual bool f_IsOpen() const = 0;
		virtual bool f_IsRunning() const = 0;
		virtual void f_SendStdIn(NMib::NStr::CStrSecure const &_Data) const = 0;
		virtual void f_CloseStdIn() const = 0;
		virtual fp64 f_GetRunningTime() const = 0;

		virtual CProcessStatistics f_GetExecutionStatistics() const
		{
			return CProcessStatistics();
		}
		virtual CProcessStatistics f_GetMemoryStatistics() const
		{
			return CProcessStatistics();
		}

		virtual CProcessStatistics f_GetOverallExecutionStatistics() const
		{
			return CProcessStatistics();
		}
		virtual CProcessStatistics f_GetOverallMemoryStatistics() const
		{
			return CProcessStatistics();
		}

	};

	class CVirtualProcessLaunch_Default : public CVirtualProcessLaunch
	{
		CProcessLaunch m_Launch;
	public:
		CVirtualProcessLaunch_Default(CProcessLaunchParams const &_Params, EProcessLaunchCloseFlag _DestructFlags);
		~CVirtualProcessLaunch_Default();
		CVirtualProcessLaunch_Default(CVirtualProcessLaunch_Default &&_Other);

		EProcessLaunchCloseFlag f_GetCloseFlags() const override;
		void f_Close(EProcessLaunchCloseFlag _CloseFlags) override;
		void f_StopProcess() const override;
		bool f_IsOpen() const override;
		bool f_IsRunning() const override;
		void f_SendStdIn(NMib::NStr::CStrSecure const &_Data) const override;
		void f_CloseStdIn() const override;
		fp64 f_GetRunningTime() const override;
		CProcessStatistics f_GetExecutionStatistics() const override;
		CProcessStatistics f_GetMemoryStatistics() const override;
		CProcessStatistics f_GetOverallExecutionStatistics() const override;
		CProcessStatistics f_GetOverallMemoryStatistics() const override;
	};

	using FVirtualProcessLaunchFactory
		= NFunction::TCFunction<NStorage::TCUniquePointer<CVirtualProcessLaunch> (CProcessLaunchParams const &_Params, EProcessLaunchCloseFlag _DestructFlags)>
	;

	class CProcessLaunchHandler
	{
	public:
		class CLaunchInfo
		{
			CProcessLaunchParams m_LaunchParams;
			NStorage::TCUniquePointer<CVirtualProcessLaunch> m_pProcessLaunch;
			NAtomic::TCAtomic<smint> m_Done;

			FVirtualProcessLaunchFactory m_Factory;

			bool fp_Lingering() const;

			NThread::CMutual m_DelayedOutputLock;
			struct CDelayedOutput
			{
				EProcessLaunchOutputType m_Type;
				NStr::CStr m_Output;
			};
			NContainer::TCLinkedList<CDelayedOutput> m_DelayedOutput;
			void fp_Clear();

		public:
			friend class CProcessLaunchHandler;

			CLaunchInfo(CProcessLaunchParams const &_Params, FVirtualProcessLaunchFactory const &_Factory);

			CVirtualProcessLaunch *f_GetProcessLaunch() const;
		};

	private:
		NThread::CEventAutoReset m_LaunchChanged;

		NContainer::TCLinkedList<CLaunchInfo> m_Launches;

		struct CDestroyed
		{
			NStorage::CIntrusiveRefCount m_RefCount;

			NThread::CMutual m_DestroyLock;
			NAtomic::TCAtomic<smint> m_Destroyed;
			CDestroyed()
				: m_Destroyed(0)
			{
			}
		};

		NStorage::TCSharedPointer<CDestroyed> m_pDestroyedNotification;

	public:
		CProcessLaunchHandler();
		~CProcessLaunchHandler();

		CLaunchInfo *f_AddLaunch(CProcessLaunchParams const &_Params, bool _bDelayOutput, FVirtualProcessLaunchFactory const &_LaunchFactory = FVirtualProcessLaunchFactory());

		void f_TerminateAll(bool _bBlock = false, NContainer::TCVector<CProcessStatistics> *o_pMemoryStats = nullptr);
		void f_StopAll();
		bool f_BlockOnExit(fp32 _Timeout = 0.0f, mint _nMaxRunning = 0, NContainer::TCVector<CProcessStatistics> *o_pMemoryStats = nullptr);
		bool f_WaitForChange(fp32 _Timeout = 0.0f);
		CLaunchInfo *f_GetFirstNotDone();

	};
}

#ifndef DMibPNoShortCuts
	using namespace NMib::NProcess;
#endif
