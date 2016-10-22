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
			NConcurrency::TCActorSubscriptionManager<void (CProcessLaunchStateChangeVariant const &_State, fp64 _TimeSinceStart)> m_OnStateChange;
			NConcurrency::TCActorSubscriptionManager<void (EProcessLaunchOutputType _OutputType, NMib::NStr::CStr const &_Output)> m_OnOutput;
			
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

		CProcessLaunchActor::CLaunch::CLaunch(CProcessLaunchParams const &_Params)
			: m_Params{_Params} 
		{
		}

		CProcessLaunchActor::CSimpleLaunch::CSimpleLaunch(NStr::CStr const &_Executable)
			: m_Executable{_Executable}
			, CLaunch{CProcessLaunchParams{}}
		{
		}

		NConcurrency::TCContinuation<CProcessLaunchActor::CSimpleLaunchResult> CProcessLaunchActor::f_LaunchSimple(CSimpleLaunch const &_SimpleLaunch)
		{
			if (_SimpleLaunch.m_Params.m_fOnOutput)
				return DMibErrorInstance("On output cannot be specified for simple launch");
			if (_SimpleLaunch.m_Params.m_fOnStateChange)
				return DMibErrorInstance("On state change cannot be specified for simple launch");

			struct CState
			{
				NConcurrency::CActorSubscription m_Subscription;
				CProcessLaunchActor::CSimpleLaunchResult m_LaunchResult;
				NConcurrency::TCContinuation<CProcessLaunchActor::CSimpleLaunchResult> m_Continuation;
			};
			
			NPtr::TCSharedPointer<CState> pState = fg_Construct();
			
			CLaunch Params{_SimpleLaunch};
			Params.m_Params.m_Target = _SimpleLaunch.m_Executable;
			Params.m_Params.m_Parameters = NMib::NProcess::CProcessLaunchParams::fs_GetParams(_SimpleLaunch.m_CommandLineParams);
			Params.m_Params.m_bSeparateStdErr = true;
			Params.m_Params.m_bAllowExecutableLocate = true;
			Params.m_Params.m_WorkingDirectory = _SimpleLaunch.m_WorkingDirectory;
			Params.m_Params.m_fOnStateChange = [this, pState](CProcessLaunchStateChangeVariant const &_StateChange, fp64 _TimeSinceLaunch)
				{
					switch (_StateChange.f_GetTypeID())
					{
					case EProcessLaunchState_Launched:
						{
						}
						break;
					case EProcessLaunchState_LaunchFailed:
						{
							pState->m_Continuation.f_SetException(DMibErrorInstance(fg_Format("Launch failed: {}", _StateChange.f_Get<EProcessLaunchState_LaunchFailed>())));
							pState->m_Subscription.f_Clear();
						}
						break;
					case EProcessLaunchState_Exited:
						{
							int32 ExitCode = _StateChange.f_Get<EProcessLaunchState_Exited>();
							pState->m_LaunchResult.m_ExitCode = ExitCode;
							pState->m_Continuation.f_SetResult(fg_Move(pState->m_LaunchResult));
							pState->m_Subscription.f_Clear();
						}
						break;
					}
				}
			;
			
			Params.m_Params.m_bSeparateStdErr = true;
			
			Params.m_Params.m_fOnOutput = [pState](EProcessLaunchOutputType _OutputType, NMib::NStr::CStr const &_Output)
				{
					auto &OutputEntry = pState->m_LaunchResult.m_Output.f_Insert();
					OutputEntry.m_Type = _OutputType;
					OutputEntry.m_Output = _Output;
				}
			;
			
			f_Launch(Params, fg_ThisActor(this)) > pState->m_Continuation / [pState](NConcurrency::CActorSubscription &&_Subscription)
				{
					pState->m_Subscription = fg_Move(_Subscription); 
				}
			;
			
			return pState->m_Continuation;
		}
		
		NConcurrency::TCContinuation<NConcurrency::CActorSubscription> CProcessLaunchActor::f_Launch
			(
				CLaunch const &_Launch
				, NConcurrency::TCActor<NConcurrency::CActor> &&_CallbackActor
			)
		{
			DMibRequire(!mp_pInternal);

			mp_pInternal = fg_Construct(this);
			auto &Internal = *mp_pInternal;
			Internal.m_DestructFlags = _Launch.m_DestructFlags;
			
			struct CState
			{
				NMib::NLog::CSysLogCatScope f_LogScope() const
				{
					return NMib::NLog::CSysLogCatScope{NMib::fg_GetSys()->f_GetLogger(), m_LogName};
				}
				
				ELogFlag m_ToLog;
				NStr::CStr m_LogName;
			};
			
			NPtr::TCSharedPointer<CState> pState = fg_Construct();
			
			if (_Launch.m_ToLog)
			{
				pState->m_ToLog = _Launch.m_ToLog; 
				if (_Launch.m_LogName.f_IsEmpty())
					pState->m_LogName = NFile::CFile::fs_GetFileNoExt(_Launch.m_Params.m_Target);
				else
					pState->m_LogName = _Launch.m_LogName;
			}
			
			CProcessLaunchParams Params = _Launch.m_Params;
			
			Params.m_fDispatcher.f_Clear();
			
			NPtr::TCUniquePointer<NConcurrency::CCombinedCallbackReference> pCombinedReference = fg_Construct();
			
			bool bOnStateChangeRegistered = false;
			if (Params.m_fOnStateChange)
			{
				bOnStateChangeRegistered = true;
				pCombinedReference->m_References.f_Insert(Internal.m_OnStateChange.f_Register(_CallbackActor, fg_Move(Params.m_fOnStateChange)));
			}
			
			Params.m_fOnStateChange = [this, ThisWeak = fg_ThisActor(this).f_Weak(), bOnStateChangeRegistered, pState](CProcessLaunchStateChangeVariant const &_State, fp64 _TimeSinceStart)
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
								[this, _TimeSinceStart, bOnStateChangeRegistered, State = _State, pState]
								{
									auto &Internal = *mp_pInternal;
									
									if (bOnStateChangeRegistered)
										Internal.m_OnStateChange(State, _TimeSinceStart);
									
									switch (State.f_GetTypeID())
									{
									case NProcess::EProcessLaunchState_Exited:
										{
											uint32 ExitCode = State.f_Get<NProcess::EProcessLaunchState_Exited>();
											
											if (!ExitCode)
											{
												if (pState->m_ToLog & ELogFlag_Info)
												{
													auto LogScope = pState->f_LogScope(); 
													DMibLog(Info, "Launch finished successfully");
												}
											}
											else
											{
												if (pState->m_ToLog & ELogFlag_Error)
												{
													auto LogScope = pState->f_LogScope(); 
													DMibLog(Error, "Launch exited with error code: {}", ExitCode);
												}
											}
											
											for (auto &Pending : Internal.m_PendingProcessStops)
												Pending.m_fOnStop(ExitCode);
											
											Internal.m_PendingProcessStops.f_Clear();
											Internal.m_bProcessExited = true;
										}
										break;
									case NProcess::EProcessLaunchState_Launched:
										{
											if (pState->m_ToLog & ELogFlag_Info)
											{
												auto LogScope = pState->f_LogScope(); 
												DMibLog(Info, "Launched");
											}
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
											if (pState->m_ToLog & ELogFlag_Error)
											{
												auto LogScope = pState->f_LogScope(); 
												DMibLog(Error, "Launch failed: {}", State.f_Get<EProcessLaunchState_LaunchFailed>());
											}
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
				
				Params.m_fOnOutput = [this, ThisWeak = fg_ThisActor(this).f_Weak(), pState](EProcessLaunchOutputType _OutputType, NMib::NStr::CStr const &_Output)
					{
						if (_OutputType == EProcessLaunchOutputType_StdOut)
						{
							if (pState->m_ToLog & ELogFlag_StdOut)
							{
								auto LogScope = pState->f_LogScope(); 
								DMibLog(Info, "{}", _Output.f_TrimRight());
							}
						}
						else
						{
							if (pState->m_ToLog & ELogFlag_Error)
							{
								auto LogScope = pState->f_LogScope(); 
								DMibLog(Error, "{}", _Output.f_TrimRight());
							}
						}
						
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
			
			NConcurrency::TCContinuation<NConcurrency::CActorSubscription> Continuation;

			if (pState->m_ToLog & ELogFlag_Info)
			{
				auto LogScope = pState->f_LogScope(); 
				DMibLog(Info, "Launching");
			}
			
			try
			{
				Internal.m_pProcessLaunch = fg_Construct(Params, _Launch.m_DestructFlags);
				Continuation.f_SetResult(fg_Move(pCombinedReference));
			}
			catch (NException::CException const &_Exception)
			{
				if (pState->m_ToLog & ELogFlag_Error)
				{
					auto LogScope = pState->f_LogScope(); 
					DMibLog(Error, "Exception launching: {}", _Exception.f_GetErrorStr());
				}
				Continuation.f_SetCurrentException();
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

