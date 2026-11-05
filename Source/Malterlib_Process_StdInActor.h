// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Process/StdIn>
#include <Mib/Concurrency/ActorFunctor>

#include "Malterlib_Process_StdIn.h"

namespace NMib::NProcess
{
	struct CStdInActor : public NConcurrency::CActor
	{
		using FOnInput = NConcurrency::TCActorFunctor<NConcurrency::TCFuture<void> (EStdInReaderOutputType _Type, NStr::CStrIO _Input)>;
		using FOnBinaryInput = NConcurrency::TCActorFunctor<NConcurrency::TCFuture<void> (EStdInReaderOutputType _Type, NContainer::CIOByteVector _Input, NStr::CStr _Error)>;

		CStdInActor();
		~CStdInActor();

		NConcurrency::TCFuture<NConcurrency::CActorSubscription> f_RegisterForInput(FOnInput _fOnInput, EStdInReaderFlag _Flags, umint _MaxSize);
		NConcurrency::TCFuture<NConcurrency::CActorSubscription> f_RegisterForInputBinary(FOnBinaryInput _fOnInput, EStdInReaderFlag _Flags, umint _MaxSize);

		NConcurrency::TCFuture<NContainer::CIOByteVector> f_ReadBinary();
		NConcurrency::TCFuture<NStr::CStrIO> f_ReadLine();
		NConcurrency::TCFuture<NStr::CStrIO> f_ReadPrompt(CStdInReaderPromptParams _Params);

		void f_AbortReads();

		static constexpr NConcurrency::EPriority mc_Priority = NConcurrency::EPriority_NormalHighCPU;

	private:
		struct CInternal;

		NConcurrency::TCFuture<void> fp_Destroy() override;

		NStorage::TCUniquePointer<CInternal> mp_pInternal;
	};
}

#ifndef DMibPNoShortCuts
using namespace NMib::NProcess;
#endif
