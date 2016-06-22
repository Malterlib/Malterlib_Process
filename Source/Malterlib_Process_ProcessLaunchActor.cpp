// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include "Malterlib_Process_ProcessLaunchActor.h"
#include <Mib/Concurrency/ActorCallbackManager>

namespace NMib
{
	namespace NProcess
	{
		
		struct CProcessLaunchActor::CInternal
		{
			NPtr::TCSharedPointer<CProcessLaunch> m_pProcessLaunch;
			NConcurrency::TCActorCallbackManager<void (CProcessLaunchStateChangeVariant const &_State, fp64 _TimeSinceStart)> m_OnStateChange;
			NConcurrency::TCActorCallbackManager<void (EProcessLaunchOutputType _OutputType, NMib::NStr::CStr const &_Output)> m_OnOutput;
			
			struct CPendingStop
			{
				NFunction::TCFunction<void (uint32 _ExitCode)> m_fOnStop;
				NFunction::TCFunction<void ()> m_fOnException;
				bool m_bStopRun;
			};
			
			NContainer::TCLinkedList<CPendingStop> m_PendingProcessStops;

			NConcurrency::TCActor<NConcurrency::CSeparateThreadActor> m_BlockingActor;
			
			EProcessLaunchCloseFlag m_DestructFlags;
			
			bool m_bProcessRunning = false;
			bool m_bProcessExited = false;
			
			CInternal(NConcurrency::CActor *_pActor)
				: m_OnStateChange(_pActor, false)
				, m_OnOutput(_pActor, false)
			{
			}

			NConcurrency::TCContinuation<void> f_RunBlocking(NFunction::TCFunction<void (NPtr::TCSharedPointer<CProcessLaunch> const &_pProcessLaunch)> const &_fSend);
		};
		
		CProcessLaunchActor::CProcessLaunchActor()
		{
		}
		
		CProcessLaunchActor::~CProcessLaunchActor()
		{
		}
		
		NConcurrency::TCContinuation<void> CProcessLaunchActor::f_Destroy()
		{
			auto &Internal = *mp_pInternal;
			if (!Internal.m_pProcessLaunch || Internal.m_bProcessExited)
			{
				Internal.m_OnStateChange.f_Clear();
				Internal.m_OnOutput.f_Clear();
				Internal.m_PendingProcessStops.f_Clear();
				return NConcurrency::TCContinuation<void>::fs_Finished();
			}
			
			NConcurrency::TCContinuation<void> Continuation;

			bool bStopRun = false;
			if (Internal.m_bProcessRunning)
			{
				try
				{
					Internal.m_pProcessLaunch->f_StopProcess();
				}
				catch (NException::CException const &)
				{
					Continuation.f_SetResult();
					return Continuation;
				}
				bStopRun = true;
			}
			auto &Pending = Internal.m_PendingProcessStops.f_Insert();
			Pending.m_bStopRun = bStopRun;
			Pending.m_fOnStop = [Continuation](uint32 _ExitCode)
				{
					Continuation.f_SetResult();
				}
			;
			Pending.m_fOnException = [Continuation]()
				{
					Continuation.f_SetCurrentException();
				}
			;
			return Continuation;
		}
		
		NConcurrency::TCContinuation<NConcurrency::CActorCallback> CProcessLaunchActor::f_Launch
			(
				CProcessLaunchParams const &_Params
				, EProcessLaunchCloseFlag _DestructFlags
				, NConcurrency::TCActor<NConcurrency::CActor> &&_CallbackActor
			)
		{
			DMibRequire(!mp_pInternal);

			mp_pInternal = fg_Construct(this);
			auto &Internal = *mp_pInternal;
			Internal.m_DestructFlags = _DestructFlags;
			
			CProcessLaunchParams Params = _Params;
			
			Params.m_fDispatcher.f_Clear();
			
			NPtr::TCUniquePointer<NConcurrency::CCombinedCallbackReference> pCombinedReference = fg_Construct();
			
			bool bOnStateChangeRegistered = false;
			if (Params.m_fOnStateChange)
			{
				bOnStateChangeRegistered = true;
				pCombinedReference->m_References.f_Insert(Internal.m_OnStateChange.f_Register(_CallbackActor, fg_Move(Params.m_fOnStateChange)));
			}
			
			Params.m_fOnStateChange = [this, ThisWeak = fg_ThisActor(this).f_Weak(), bOnStateChangeRegistered](CProcessLaunchStateChangeVariant const &_State, fp64 _TimeSinceStart)
				{
					auto ThisActor = ThisWeak.f_Lock();
					if (!ThisActor)
						return; // Already deleted
					
					ThisActor
						(
							&CActor::f_Dispatch
							, NFunction::TCFunction<void ()>
							(
								//[this, _State, _TimeSinceStart, bOnStateChangeRegistered]
								[this, _TimeSinceStart, bOnStateChangeRegistered, State = _State]
								{
									auto &Internal = *mp_pInternal;
									
									if (bOnStateChangeRegistered)
										Internal.m_OnStateChange(State, _TimeSinceStart);
									
									switch (State.f_GetTypeID())
									{
									case NProcess::EProcessLaunchState_Exited:
										{
											uint32 ExitCode = State.f_Get<NProcess::EProcessLaunchState_Exited>();
											
											for (auto &Pending : Internal.m_PendingProcessStops)
												Pending.m_fOnStop(ExitCode);
											
											Internal.m_PendingProcessStops.f_Clear();
											Internal.m_bProcessExited = true;
										}
										break;
									case NProcess::EProcessLaunchState_Launched:
										{
											Internal.m_bProcessRunning = true;
											if (Internal.m_pProcessLaunch)
											{
												for (auto iPending = Internal.m_PendingProcessStops.f_GetIterator(); iPending;)
												{
													auto &Pending = *iPending;
													++iPending;
													if (Pending.m_bStopRun)
														continue;
													try
													{
														Internal.m_pProcessLaunch->f_StopProcess();
													}
													catch (...)
													{
														Pending.m_fOnException();
														Internal.m_PendingProcessStops.f_Remove(Pending);
														continue;
													}
													Pending.m_bStopRun = true;
												}
											}
										}
										break;
									case NProcess::EProcessLaunchState_LaunchFailed:
										{
											for (auto &Pending : Internal.m_PendingProcessStops)
												Pending.m_fOnStop(-1);
											Internal.m_PendingProcessStops.f_Clear();
											Internal.m_bProcessExited = true;
										}
										break;
									}
								}
							)
						)
						> NConcurrency::fg_DiscardResult()
					;
				}
			;
			
			if (Params.m_fOnOutput)
			{
				pCombinedReference->m_References.f_Insert(Internal.m_OnOutput.f_Register(_CallbackActor, fg_Move(Params.m_fOnOutput)));
				
				Params.m_fOnOutput = [this, ThisWeak = fg_ThisActor(this).f_Weak()](EProcessLaunchOutputType _OutputType, NMib::NStr::CStr const &_Output)
					{
						auto ThisActor = ThisWeak.f_Lock();
						if (!ThisActor)
							return; // Already deleted
						
						ThisActor
							(
								&CActor::f_Dispatch
								, [this, _OutputType, _Output]
								{
									auto &Internal = *mp_pInternal;
									Internal.m_OnOutput(_OutputType, _Output);
								}
							) 
							> NConcurrency::fg_DiscardResult()
						;
						
					}
				;
			}
			
			NConcurrency::TCContinuation<NConcurrency::CActorCallback> Continuation;
			
			try
			{
				Internal.m_pProcessLaunch = fg_Construct(Params, _DestructFlags);
				Continuation.f_SetResult(fg_Move(pCombinedReference));
			}
			catch (...)
			{
				Continuation.f_SetCurrentException();
			}			
			
			return Continuation;
		}
		
		NConcurrency::TCContinuation<void> CProcessLaunchActor::CInternal::f_RunBlocking
			(
				NFunction::TCFunction<void (NPtr::TCSharedPointer<CProcessLaunch> const &_pProcessLaunch)> const &_fSend
			)
		{
			if (!m_pProcessLaunch)
				return fg_Explicit();
			
			if (!m_BlockingActor)
				m_BlockingActor = NConcurrency::fg_ConstructActor<NConcurrency::CSeparateThreadActor>(fg_Construct("Blocking process launch"));
			
			NConcurrency::TCContinuation<void> Continuation;
			// Dispatch on separate thread actor as this can block
			fg_Dispatch
				(
					m_BlockingActor
					, [_fSend, pProcessLaunch = m_pProcessLaunch]()
					{
						_fSend(pProcessLaunch);
					}
				)
				> [Continuation](NConcurrency::TCAsyncResult<void> &&_Result)
				{
					Continuation.f_SetResult(fg_Move(_Result));
				}
			;
			
			return Continuation;
		}

		NConcurrency::TCContinuation<void> CProcessLaunchActor::f_SendStdInBinary(NContainer::TCVector<uint8, NMem::CAllocator_HeapSecure> const &_Data) const
		{
			auto &Internal = *mp_pInternal;
			return Internal.f_RunBlocking
				(
					[_Data](NPtr::TCSharedPointer<CProcessLaunch> const &_pProcessLaunch)
					{
						_pProcessLaunch->f_SendStdInBinary(_Data);
					}
				)
			;
		}
		
		NConcurrency::TCContinuation<void> CProcessLaunchActor::f_SendStdIn(NMib::NStr::CStr const &_Data) const
		{
			auto &Internal = *mp_pInternal;
			return Internal.f_RunBlocking
				(
					[_Data](NPtr::TCSharedPointer<CProcessLaunch> const &_pProcessLaunch)
					{
						_pProcessLaunch->f_SendStdIn(_Data);
					}
				)
			;
		}
		
		NConcurrency::TCContinuation<uint32> CProcessLaunchActor::f_StopProcess() const
		{
			auto &Internal = *mp_pInternal;
			if (!Internal.m_pProcessLaunch)
				return NConcurrency::TCContinuation<uint32>::fs_Finished(0);
			
			NConcurrency::TCContinuation<uint32> Continuation;
			
			bool bStopRun = false;

			if (Internal.m_bProcessRunning)
			{
				try
				{
					Internal.m_pProcessLaunch->f_StopProcess();
				}
				catch (NException::CException const &)
				{
					Continuation.f_SetCurrentException();
					return Continuation;
				}
				bStopRun = true;
			}
			auto &Pending = Internal.m_PendingProcessStops.f_Insert();
			Pending.m_bStopRun = bStopRun;
			Pending.m_fOnStop = [Continuation](uint32 _ExitCode)
				{
					Continuation.f_SetResult(_ExitCode);
				}
			;
			Pending.m_fOnException = [Continuation]()
				{
					Continuation.f_SetCurrentException();
				}
			;
			return Continuation;
		}
		
		template <typename tf_CType, typename tf_FToWrap>
		NConcurrency::TCContinuation<tf_CType> fg_WrapContinuation(tf_FToWrap &&_fToWrap)
		{
			NConcurrency::TCContinuation<tf_CType> Continuation;
			try
			{
				Continuation.f_SetResult(_fToWrap());
			}
			catch (...)
			{
				Continuation.f_SetCurrentException();
			}
			return Continuation;
		}
		
		NConcurrency::TCContinuation<fp64> CProcessLaunchActor::f_GetRunningTime() const
		{
			return fg_WrapContinuation<fp64>
				(
					[&]()
					{
						auto &Internal = *mp_pInternal;
						DMibRequire(Internal.m_pProcessLaunch);
						return Internal.m_pProcessLaunch->f_GetRunningTime();
					}
				)
			;
		}
		
		NConcurrency::TCContinuation<CProcessStatistics> CProcessLaunchActor::f_GetExecutionStatistics() const
		{
			return fg_WrapContinuation<CProcessStatistics>
				(
					[&]()
					{
						auto &Internal = *mp_pInternal;
						DMibRequire(Internal.m_pProcessLaunch);
						return Internal.m_pProcessLaunch->f_GetExecutionStatistics();
					}
				)
			;
		}
		
		NConcurrency::TCContinuation<CProcessStatistics> CProcessLaunchActor::f_GetMemoryStatistics() const
		{
			return fg_WrapContinuation<CProcessStatistics>
				(
					[&]()
					{
						auto &Internal = *mp_pInternal;
						DMibRequire(Internal.m_pProcessLaunch);
						return Internal.m_pProcessLaunch->f_GetMemoryStatistics();
					}
				)
			;
		}
		
		NConcurrency::TCContinuation<CProcessStatistics> CProcessLaunchActor::f_GetOverallExecutionStatistics() const
		{
			return fg_WrapContinuation<CProcessStatistics>
				(
					[&]()
					{
						auto &Internal = *mp_pInternal;
						DMibRequire(Internal.m_pProcessLaunch);
						return Internal.m_pProcessLaunch->f_GetOverallExecutionStatistics();
					}
				)
			;
		}
		
		NConcurrency::TCContinuation<CProcessStatistics> CProcessLaunchActor::f_GetOverallMemoryStatistics() const
		{
			return fg_WrapContinuation<CProcessStatistics>
				(
					[&]()
					{
						auto &Internal = *mp_pInternal;
						DMibRequire(Internal.m_pProcessLaunch);
						return Internal.m_pProcessLaunch->f_GetOverallMemoryStatistics();
					}
				)
			;
		}
	}
}

