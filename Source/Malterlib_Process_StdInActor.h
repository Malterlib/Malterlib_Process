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
	class CStdInActor : public NConcurrency::CActor
	{
	public:
		CStdInActor();
		~CStdInActor();

		using FOnInput = NConcurrency::TCActorFunctor<NConcurrency::TCFuture<void> (EStdInReaderOutputType _Type, NStr::CStrIO _Input)>;
		NConcurrency::TCFuture<NConcurrency::CActorSubscription> f_RegisterForInput(FOnInput _fOnInput, EStdInReaderFlag _Flags, umint _MaxSize);

		using FOnBinaryInput
			= NConcurrency::TCActorFunctor<NConcurrency::TCFuture<void> (EStdInReaderOutputType _Type, NContainer::CIOByteVector _Input, NStr::CStr _Error)>
		;
		NConcurrency::TCFuture<NConcurrency::CActorSubscription> f_RegisterForInputBinary(FOnBinaryInput _fOnInput, EStdInReaderFlag _Flags, umint _MaxSize);

		NConcurrency::TCFuture<NContainer::CIOByteVector> f_ReadBinary();
		NConcurrency::TCFuture<NStr::CStrIO> f_ReadLine();
		NConcurrency::TCFuture<NStr::CStrIO> f_ReadPrompt(CStdInReaderPromptParams _Params);

		void f_AbortReads();

	private:
		NConcurrency::TCFuture<void> fp_Destroy() override;

		struct CInternal;

		NStorage::TCUniquePointer<CInternal> mp_pInternal;
	};
}

#ifndef DMibPNoShortCuts
	using namespace NMib::NProcess;
#endif
