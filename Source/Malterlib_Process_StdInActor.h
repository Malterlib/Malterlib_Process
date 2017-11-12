// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

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

		using FOnInput = NConcurrency::TCActorFunctor<NConcurrency::TCContinuation<void> (EStdInReaderOutputType _Type, NStr::CStrSecure const &_Input)>;
		NConcurrency::TCContinuation<NConcurrency::CActorSubscription> f_RegisterForInput(FOnInput &&_fOnInput, EStdInReaderFlag _Flags);

		using FOnBinaryInput
			= NConcurrency::TCActorFunctor<NConcurrency::TCContinuation<void> (EStdInReaderOutputType _Type, NContainer::CSecureByteVector const &_Input, NStr::CStr const &_Error)>
		;
		NConcurrency::TCContinuation<NConcurrency::CActorSubscription> f_RegisterForInputBinary(FOnBinaryInput &&_fOnInput, EStdInReaderFlag _Flags);

		NConcurrency::TCContinuation<NContainer::CSecureByteVector> f_ReadBinary();
		NConcurrency::TCContinuation<NStr::CStrSecure> f_ReadLine();
		NConcurrency::TCContinuation<NStr::CStrSecure> f_ReadPrompt(CStdInReaderPromptParams const &_Params);

		void f_AbortReads();

	private:
		NConcurrency::TCContinuation<void> fp_Destory();

		struct CInternal;

		NPtr::TCUniquePointer<CInternal> mp_pInternal;
	};
}

#ifndef DMibPNoShortCuts
	using namespace NMib::NProcess;	
#endif
