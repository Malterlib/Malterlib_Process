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

		using FOnInput = NConcurrency::TCActorFunctor<NConcurrency::TCContinuation<void> (EStdInReaderOutputType _Type, NStr::CStr const &_Input)>;
		NConcurrency::TCContinuation<NConcurrency::CActorSubscription> f_RegisterForInput(FOnInput &&_fOnInput, EStdInReaderFlag _Flags);

	private:
		struct CInternal;
		
		NPtr::TCUniquePointer<CInternal> mp_pInternal;
	};
}

#ifndef DMibPNoShortCuts
	using namespace NMib::NProcess;	
#endif
