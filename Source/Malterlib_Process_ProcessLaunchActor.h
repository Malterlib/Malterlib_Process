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
			CProcessLaunchActor();
			~CProcessLaunchActor();
			
			NConcurrency::TCContinuation<NConcurrency::CActorCallback> f_Launch
				(
					CProcessLaunchParams const &_Params
					, EProcessLaunchCloseFlag _DestructFlags
					, NConcurrency::TCActor<NConcurrency::CActor> &&_CallbackActor
				)
			;

			NConcurrency::TCContinuation<void> f_SendStdIn(NMib::NStr::CStr const &_Data) const;
			NConcurrency::TCContinuation<void> f_SendStdInBinary(NContainer::TCVector<uint8, NMem::CAllocator_HeapSecure> const &_Data) const;
			NConcurrency::TCContinuation<uint32> f_StopProcess() const; // Soft termination
			
			NConcurrency::TCContinuation<fp64> f_GetRunningTime() const;
			
			NConcurrency::TCContinuation<CProcessStatistics> f_GetExecutionStatistics() const;
			NConcurrency::TCContinuation<CProcessStatistics> f_GetMemoryStatistics() const;
			NConcurrency::TCContinuation<CProcessStatistics> f_GetOverallExecutionStatistics() const;
			NConcurrency::TCContinuation<CProcessStatistics> f_GetOverallMemoryStatistics() const;
			
			NConcurrency::TCContinuation<void> f_Destroy() override;

		private:
			struct CInternal;
			
			NPtr::TCUniquePointer<CInternal> mp_pInternal;
		};
	}
}

#ifndef DMibPNoShortCuts
	using namespace NMib::NProcess;	
#endif

