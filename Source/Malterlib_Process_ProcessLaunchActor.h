// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Process/ProcessLaunch>

namespace NMib
{
	namespace NProcess
	{
		class CProcessLaunchActor : public NConcurrency::CActor
		{
		public:
			enum ELogFlag
			{
				ELogFlag_None = 0
				, ELogFlag_Error = DMibBit(0) 
				, ELogFlag_StdOut = DMibBit(1)
				, ELogFlag_StdErr = DMibBit(2) 
				, ELogFlag_Info = DMibBit(3)
				, ELogFlag_AdditionallyOutputToStdErr = DMibBit(4)
				, ELogFlag_All = ELogFlag_Error | ELogFlag_StdOut | ELogFlag_StdErr | ELogFlag_Info  
			};
			
			enum ESimpleLaunchFlag
			{
				ESimpleLaunchFlag_None = 0
				, ESimpleLaunchFlag_GenerateExceptionOnNonZeroExitCode = DMibBit(0)
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
				NStr::CStr f_GetErrorOut() const;
				NStr::CStr f_GetCombinedOut() const;
			};
			
			CProcessLaunchActor();
			~CProcessLaunchActor();

			NConcurrency::TCContinuation<NConcurrency::CActorSubscription> f_Launch
				(
					CLaunch const &_Launch
					, NConcurrency::TCActor<NConcurrency::CActor> &&_CallbackActor
				)
			;

			NConcurrency::TCContinuation<CSimpleLaunchResult> f_LaunchSimple(CSimpleLaunch const &_SimpleLaunch);
			
			NConcurrency::TCContinuation<void> f_SendStdIn(NMib::NStr::CStrSecure const &_Data) const;
			NConcurrency::TCContinuation<void> f_SendStdInBinary(NContainer::TCVector<uint8, NMem::CAllocator_HeapSecure> const &_Data) const;
			NConcurrency::TCContinuation<uint32> f_StopProcess() const; // Soft termination
			
			NConcurrency::TCContinuation<fp64> f_GetRunningTime() const;
			
			NConcurrency::TCContinuation<CProcessStatistics> f_GetExecutionStatistics() const;
			NConcurrency::TCContinuation<CProcessStatistics> f_GetMemoryStatistics() const;
			NConcurrency::TCContinuation<CProcessStatistics> f_GetOverallExecutionStatistics() const;
			NConcurrency::TCContinuation<CProcessStatistics> f_GetOverallMemoryStatistics() const;
			

		protected:
			NConcurrency::TCContinuation<void> fp_Destroy() override;
			virtual bool fp_WillFilterOutput();
			virtual void fp_FilterOutput(EProcessLaunchOutputType _OutputType, NMib::NStr::CStr &o_Output);
			virtual void fp_ModifyLaunch(CLaunch &o_Launch);
			
		private:
			struct CInternal;
			
			NPtr::TCUniquePointer<CInternal> mp_pInternal;
		};
	}
}

#ifndef DMibPNoShortCuts
	using namespace NMib::NProcess;	
#endif

