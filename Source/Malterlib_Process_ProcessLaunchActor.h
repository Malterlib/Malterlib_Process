// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Process/ProcessLaunch>

namespace NMib::NProcess
{
	class CProcessLaunchActor : public NConcurrency::CActor
	{
	public:
		enum ELogFlag
		{
			ELogFlag_None = 0
			, ELogFlag_OtherError = DMibBit(0)
			, ELogFlag_StdOut = DMibBit(1)
			, ELogFlag_StdErr = DMibBit(2)
			, ELogFlag_Info = DMibBit(3)
			, ELogFlag_AdditionallyOutputToStdErr = DMibBit(4)
			, ELogFlag_AdditionallyOutputToStdErrDirectStdErr = DMibBit(5)
			, ELogFlag_ErrorExit = DMibBit(6)
			, ELogFlag_Error = ELogFlag_OtherError | ELogFlag_ErrorExit
			, ELogFlag_All = ELogFlag_Error | ELogFlag_StdOut | ELogFlag_StdErr | ELogFlag_Info
		};

		enum ESimpleLaunchFlag
		{
			ESimpleLaunchFlag_None = 0
			, ESimpleLaunchFlag_GenerateExceptionOnNonZeroExitCode = DMibBit(0)
			, ESimpleLaunchFlag_ForwardStdInput = DMibBit(1)
		};

		struct COutput
		{
			EProcessLaunchOutputType m_Type = EProcessLaunchOutputType_StdErr;
			NStr::CStr m_Output;
		};

		struct CLaunch
		{
			CLaunch(CProcessLaunchParams const &_Params);
			CProcessLaunchParams m_Params;

			ELogFlag m_ToLog = ELogFlag_None;
			NStr::CStr m_LogName;
			EProcessLaunchCloseFlag m_DestructFlags = EProcessLaunchCloseFlag_StopProcess | EProcessLaunchCloseFlag_BlockOnExit;
			bool m_bWholeLineOutput = true;
		};

		struct CSimpleLaunch : public CLaunch
		{
			CSimpleLaunch
				(
					NStr::CStr const &_Executable
					, NContainer::TCVector<NStr::CStr> const &_Params = {}
					, NStr::CStr const &_WorkingDir = {}
					, ESimpleLaunchFlag _Flags = ESimpleLaunchFlag_None
				)
			;
			CSimpleLaunch(CProcessLaunchParams const &_Params);

			ESimpleLaunchFlag m_SimpleFlags = ESimpleLaunchFlag_None;
		};

		struct CSimpleLaunchResult
		{
			uint32 m_ExitCode = TCLimitsInt<uint32>::mc_Max;
			NContainer::TCVector<COutput> m_Output;

			NStr::CStr f_GetStdOut() const;
			NStr::CStr f_GetStdErr() const;
			NStr::CStr f_GetErrorOut() const;
			NStr::CStr f_GetCombinedOut() const;
		};

		CProcessLaunchActor();
		~CProcessLaunchActor();

		NConcurrency::TCFuture<NConcurrency::CActorSubscription> f_Launch
			(
				CLaunch _Launch
				, NConcurrency::TCActor<NConcurrency::CActor> _CallbackActor
			)
		;

		NConcurrency::TCFuture<CSimpleLaunchResult> f_LaunchSimple(CSimpleLaunch _SimpleLaunch);

		NConcurrency::TCFuture<void> f_SendStdIn(NMib::NStr::CStrIO _Data) const;
		NConcurrency::TCFuture<void> f_CloseStdIn() const;
		NConcurrency::TCFuture<void> f_SendStdInBinary(NContainer::CIOByteVector _Data) const;
		NConcurrency::TCFuture<uint32> f_StopProcess() const; // Soft termination
		NConcurrency::TCFuture<uint32> f_StopProcessGroup() const; // Soft termination
		NConcurrency::TCFuture<void> f_Signal(int32 _Signal) const; // Only for unix

		NConcurrency::TCFuture<fp64> f_GetRunningTime() const;

		NConcurrency::TCFuture<CProcessStatistics> f_GetExecutionStatistics() const;
		NConcurrency::TCFuture<CProcessStatistics> f_GetMemoryStatistics() const;
		NConcurrency::TCFuture<CProcessStatistics> f_GetOverallExecutionStatistics() const;
		NConcurrency::TCFuture<CProcessStatistics> f_GetOverallMemoryStatistics() const;

		static NConcurrency::TCFuture<CSimpleLaunchResult> fs_LaunchSimple(CSimpleLaunch _SimpleLaunch);

	protected:
		NConcurrency::TCFuture<void> fp_Destroy() override;
		NConcurrency::TCFuture<uint32> fp_StopProcess(bool _bGroup) const;
		virtual bool fp_WillFilterOutput();
		virtual void fp_FilterOutput(EProcessLaunchOutputType _OutputType, NMib::NStr::CStr &o_Output);
		virtual void fp_ModifyLaunch(CLaunch &o_Launch);

	private:
		struct CInternal;

		NStorage::TCUniquePointer<CInternal> mp_pInternal;
	};
}

#ifndef DMibPNoShortCuts
	using namespace NMib::NProcess;
#endif
