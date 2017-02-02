// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include "Malterlib_Process_StdInActor.h"
#include <Mib/Concurrency/ActorSubscription>

namespace NMib::NProcess
{
	struct CStdInActor::CInternal
	{
		struct CSubscription
		{
			CSubscription(CStdInReaderParams &&_Params)
				: m_StdInReader(fg_Move(_Params))
			{
			}
			
			CStdInReader m_StdInReader;
		};
		
		NContainer::TCSet<NPtr::TCSharedPointer<CSubscription, NPtr::CSupportWeakTag>> m_Subscriptions;
	};
	
	CStdInActor::CStdInActor()
		: mp_pInternal(fg_Construct())
	{
	}
	
	CStdInActor::~CStdInActor()
	{
	}

	NConcurrency::TCContinuation<NConcurrency::CActorSubscription> CStdInActor::f_RegisterForInput(FOnInput &&_fOnInput, EStdInReaderFlag _Flags)
	{
		auto &Internal = *mp_pInternal;
		NConcurrency::TCContinuation<NConcurrency::CActorSubscription> Continuation;
		try
		{
			NPtr::TCSharedPointer<CInternal::CSubscription, NPtr::CSupportWeakTag> pSubscription = fg_Construct
				(
					CStdInReaderParams::fs_Create
					(
						[fOnInput = fg_Move(_fOnInput)](EStdInReaderOutputType _Type, NStr::CStr const &_Input)
						{
							fOnInput(_Type, _Input) > NConcurrency::fg_DiscardResult();
						}
						, _Flags
					)
				)
			;
			
			Internal.m_Subscriptions[pSubscription];
			
			Continuation.f_SetResult
				(
					NConcurrency::g_ActorSubscription > [this, pSubscriptionWeak = pSubscription.f_Weak()]
					{
						auto &Internal = *mp_pInternal;
						Internal.m_Subscriptions.f_Remove(pSubscriptionWeak);
					}
				)
			;
		}
		catch (NException::CException const &)
		{
			Continuation.f_SetCurrentException();
		}
		return Continuation;
	}
}
