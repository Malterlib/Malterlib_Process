// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <Mib/Core/Core>

#include "Malterlib_Process_ProcessLaunch.h"

namespace NMib::NProcess
{
	struct CVirtualProcessLaunch
	{
		virtual ~CVirtualProcessLaunch();

		virtual EProcessLaunchCloseFlag f_GetCloseFlags() const = 0;
		virtual void f_Close(EProcessLaunchCloseFlag _CloseFlags) = 0;
		virtual void f_StopProcess() const = 0;
		virtual void f_StopProcessGroup() const;
		virtual bool f_IsOpen() const = 0;
		virtual bool f_IsRunning() const = 0;
		virtual void f_SendStdIn(NMib::NStr::CStrIO const &_Data) const = 0;
		virtual void f_CloseStdIn() const = 0;
		virtual fp64 f_GetRunningTime() const = 0;

		virtual CProcessStatistics f_GetExecutionStatistics() const;
		virtual CProcessStatistics f_GetMemoryStatistics() const;
		virtual CProcessStatistics f_GetOverallExecutionStatistics() const;
		virtual CProcessStatistics f_GetOverallMemoryStatistics() const;
	};

	struct CVirtualProcessLaunch_Default : public CVirtualProcessLaunch
	{
		CVirtualProcessLaunch_Default(CProcessLaunchParams const &_Params, EProcessLaunchCloseFlag _DestructFlags);
		~CVirtualProcessLaunch_Default();
		CVirtualProcessLaunch_Default(CVirtualProcessLaunch_Default &&_Other);

		EProcessLaunchCloseFlag f_GetCloseFlags() const override;
		void f_Close(EProcessLaunchCloseFlag _CloseFlags) override;
		void f_StopProcess() const override;
		void f_StopProcessGroup() const override;
		bool f_IsOpen() const override;
		bool f_IsRunning() const override;
		void f_SendStdIn(NMib::NStr::CStrIO const &_Data) const override;
		void f_CloseStdIn() const override;
		fp64 f_GetRunningTime() const override;
		CProcessStatistics f_GetExecutionStatistics() const override;
		CProcessStatistics f_GetMemoryStatistics() const override;
		CProcessStatistics f_GetOverallExecutionStatistics() const override;
		CProcessStatistics f_GetOverallMemoryStatistics() const override;

		umint f_GetProcessID() const;

	private:
		CProcessLaunch mp_Launch;
		bool mp_bCreatedProcessGroup;
	};

	using FVirtualProcessLaunchFactory = NFunction::TCFunction
		<
			NStorage::TCUniquePointer<CVirtualProcessLaunch> (CProcessLaunchParams const &_Params, EProcessLaunchCloseFlag _DestructFlags)
		>
	;

	struct CProcessLaunchHandler
	{
		struct CLaunchInfo
		{
			CLaunchInfo(CProcessLaunchParams const &_Params, FVirtualProcessLaunchFactory const &_fFactory);

			CVirtualProcessLaunch *f_GetProcessLaunch() const;

		private:
			friend struct CProcessLaunchHandler;

			enum class ECompletion : smint
			{
				mc_Running
				, mc_Reported
				, mc_CallbackFinished
			};

			struct CDelayedOutput
			{
				NContainer::CIOByteVector m_BinaryOutput;
				NStr::CStr m_Output;
				EProcessLaunchOutputType m_Type;
				bool m_bBinary = false;
			};

			bool fp_Lingering() const;
			void fp_Clear();

			CProcessLaunchParams mp_LaunchParams;
			NStorage::TCUniquePointer<CVirtualProcessLaunch> mp_pProcessLaunch;
			NAtomic::TCAtomic<ECompletion> mp_Completion;

			FVirtualProcessLaunchFactory mp_fFactory;

			NThread::CMutual mp_DelayedOutputLock;
			NContainer::TCLinkedList<CDelayedOutput> mp_DelayedOutput;
		};

		CProcessLaunchHandler();
		~CProcessLaunchHandler();

		CLaunchInfo *f_AddLaunch(CProcessLaunchParams const &_Params, bool _bDelayOutput, FVirtualProcessLaunchFactory const &_fLaunchFactory = FVirtualProcessLaunchFactory());

		void f_TerminateAll
			(
				bool _bBlock = false
				, NContainer::TCVector<CProcessStatistics> *o_pMemoryStats = nullptr
				, EProcessLaunchCloseFlag _Termination = EProcessLaunchCloseFlag_TerminateProcess
			)
		;
		void f_StopAll(bool _bProcessGroups = false);
		void f_ReapCompleted();
		bool f_BlockOnExit(fp32 _Timeout = 0.0f, umint _nMaxRunning = 0, NContainer::TCVector<CProcessStatistics> *o_pMemoryStats = nullptr);
		bool f_WaitForChange(fp32 _Timeout = 0.0f);
		CLaunchInfo *f_GetFirstNotDone();

	private:
		struct CDestroyed
		{
			CDestroyed();

			NStorage::CIntrusiveRefCount m_RefCount;

			NThread::CMutual m_DestroyLock;
			NAtomic::TCAtomic<smint> m_Destroyed;
		};

		NThread::CEventAutoReset mp_LaunchChanged;

		NContainer::TCLinkedList<CLaunchInfo> mp_Launches;

		NStorage::TCSharedPointer<CDestroyed> mp_pDestroyedNotification;
	};
}

#ifndef DMibPNoShortCuts
using namespace NMib::NProcess;
#endif
