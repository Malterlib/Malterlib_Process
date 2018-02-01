// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include "Malterlib_Process_ProcessLaunchActor.h"
#include <Mib/Concurrency/ActorCallbackManager>

namespace NMib
{
	namespace NProcess
	{
		static NThread::CMutual g_StdOutLogLock;
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
		
		bool CProcessLaunchActor::fp_WillFilterOutput()
		{
			return false;
		}
		
		void CProcessLaunchActor::fp_FilterOutput(EProcessLaunchOutputType _OutputType, NMib::NStr::CStr &o_Output)
		{
		}
		
		void CProcessLaunchActor::fp_ModifyLaunch(CLaunch &o_Launch)
		{
		}
		
		NConcurrency::TCContinuation<void> CProcessLaunchActor::fp_Destroy()
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

		CProcessLaunchActor::CSimpleLaunch::CSimpleLaunch(CProcessLaunchParams const &_Params)
			: CLaunch{_Params}
		{
		}
		
		CProcessLaunchActor::CSimpleLaunch::CSimpleLaunch
			(
				NStr::CStr const &_Executable
				, NContainer::TCVector<NStr::CStr> const &_Params
				, NStr::CStr const &_WorkingDir
				, ESimpleLaunchFlag _Flags
			)
			: CSimpleLaunch{CProcessLaunchParams::fs_LaunchExecutable(_Executable, _Params, _WorkingDir, {})}
		{
			m_Params.m_bAllowExecutableLocate = true;
			m_Params.m_bShowLaunched = false;
			m_SimpleFlags = _Flags;
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
				ESimpleLaunchFlag m_SimpleFlags = ESimpleLaunchFlag_None;
			};
			
			NPtr::TCSharedPointer<CState> pState = fg_Construct();
			
			pState->m_SimpleFlags = _SimpleLaunch.m_SimpleFlags;
			
			CLaunch Params{_SimpleLaunch};
			Params.m_Params.m_fOnStateChange = [pState](CProcessLaunchStateChangeVariant const &_StateChange, fp64 _TimeSinceLaunch)
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
							if (ExitCode && (pState->m_SimpleFlags & ESimpleLaunchFlag_GenerateExceptionOnNonZeroExitCode))
								pState->m_Continuation.f_SetException(DMibErrorInstance(fg_Format("Launch exited with {}: {}", ExitCode, pState->m_LaunchResult.f_GetErrorOut())));
							else
							{
								pState->m_LaunchResult.m_ExitCode = ExitCode;
								pState->m_Continuation.f_SetResult(fg_Move(pState->m_LaunchResult));
							}
							pState->m_Subscription.f_Clear();
						}
						break;
					}
				}
			;
			
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

			CLaunch Launch = _Launch;
			fp_ModifyLaunch(Launch);
			
			struct CState : NPtr::TCSharedPointerIntrusiveBase<>
			{
				static NMib::NLog::CSysLogCatScope fs_LogScope(NStr::CStr const &_LogName)
				{
					return NMib::NLog::CSysLogCatScope{NMib::fg_GetSys()->f_GetLogger(), _LogName};
				}
				
				NMib::NLog::CSysLogCatScope f_LogScope() const
				{
					return NMib::NLog::CSysLogCatScope{NMib::fg_GetSys()->f_GetLogger(), m_LogName};
				}
				
				~CState()
				{
					f_FlushOutput();
				}
				
				void f_FlushOutput()
				{
					f_ProcessOutput(EProcessLaunchOutputType_StdOut, true);
					f_ProcessOutput(EProcessLaunchOutputType_StdErr, true);
					f_ProcessOutput(EProcessLaunchOutputType_GeneralError, true);
					f_ProcessOutput(EProcessLaunchOutputType_TerminateMessage, true);
				}
				
				void f_ProcessOutput(EProcessLaunchOutputType _OutputType, bool _bFlush)
				{
					if (!m_bWholeLineOutput)
						_bFlush = true;
					
					NStr::CStr Output;
					
					if (_bFlush)
						Output = fg_Move(m_OutputBuffers[_OutputType]);
					else
					{
						auto &OutputBuffer = m_OutputBuffers[_OutputType];
						auto *pParse = OutputBuffer.f_GetStr();
						auto *pFinishedOutput = pParse;
						while (*pParse)
						{
							NStr::fg_ParseToEndOfLine(pParse);
							auto *pEndOfLine = pParse;
							NStr::fg_ParseEndOfLine(pParse);
							if (pParse != pEndOfLine)
								pFinishedOutput = pParse;
							else
								break;
						}
						mint nFinishedChars = pFinishedOutput - OutputBuffer.f_GetStr();
						if (!nFinishedChars)
							return;
						Output = OutputBuffer.f_Extract(0, nFinishedChars);
						fg_StrDelete(OutputBuffer, 0, nFinishedChars);
					}
					
					if (Output.f_IsEmpty())
						return;
					
					auto ThisActor = m_ThisWeak.f_Lock();
					if (!ThisActor)
						return; // Already deleted
					
					NConcurrency::g_Dispatch(ThisActor) > [pThis = m_pThis, _OutputType, Output = fg_Move(Output), ToLog = m_ToLog, LogName = m_LogName]() mutable
						{
							auto &Internal = *pThis->mp_pInternal;
							pThis->fp_FilterOutput(_OutputType, Output);
							if (Output.f_IsEmpty())
								return;
							
							switch (_OutputType)
							{
							case EProcessLaunchOutputType_StdOut:
								{
									if (ToLog & ELogFlag_StdOut)
									{
										if (ToLog & ELogFlag_AdditionallyOutputToStdErr)
										{
											DMibLock(g_StdOutLogLock);
											DMibConErrOut2("{}: {}\n", LogName, Output.f_TrimRight());
										}
										auto LogScope = fs_LogScope(LogName);
										DMibLog(Info, "{}", Output.f_TrimRight());
									}
									break;
								}
							case EProcessLaunchOutputType_StdErr: 
								{
									if (ToLog & ELogFlag_StdErr)
									{
										if (ToLog & ELogFlag_AdditionallyOutputToStdErr)
										{
											DMibLock(g_StdOutLogLock);
											DMibConErrOut2("{}: {}\n", LogName, Output.f_TrimRight());
										}
										auto LogScope = fs_LogScope(LogName);
										DMibLog(Error, "{}", Output.f_TrimRight());
									}
									break;
								}
							default:
								{
									if (ToLog & ELogFlag_Error)
									{
										if (ToLog & ELogFlag_AdditionallyOutputToStdErr)
										{
											DMibLock(g_StdOutLogLock);
											DMibConErrOut2("{}: {}\n", LogName, Output.f_TrimRight());
										}
										auto LogScope = fs_LogScope(LogName);
										DMibLog(Error, "{}", Output.f_TrimRight());
									}
									break;
								}
							}
							
							Internal.m_OnOutput(_OutputType, Output);
						}
						> NConcurrency::fg_DiscardResult()
					;
				}
				
				ELogFlag m_ToLog = ELogFlag_None;
				NStr::CStr m_LogName;
				NStr::CStr m_OutputBuffers[EProcessLaunchOutputType_Max];
				NConcurrency::TCWeakActor<CProcessLaunchActor> m_ThisWeak;
				CProcessLaunchActor *m_pThis = nullptr;
				bool m_bWholeLineOutput = true;
			};
			
			NPtr::TCSharedPointer<CState> pState = fg_Construct();
			
			pState->m_bWholeLineOutput = Launch.m_bWholeLineOutput;
			pState->m_ThisWeak = fg_ThisActor(this);
			pState->m_pThis = this;
			pState->m_ToLog = Launch.m_ToLog; 
			
			if (Launch.m_ToLog & ELogFlag_All)
			{
				if (Launch.m_LogName.f_IsEmpty())
					pState->m_LogName = NFile::CFile::fs_GetFileNoExt(Launch.m_Params.m_Target);
				else
					pState->m_LogName = Launch.m_LogName;
			}
			
			CProcessLaunchParams Params = Launch.m_Params;
			
			Params.m_fDispatcher.f_Clear();
			
			NPtr::TCUniquePointer<NConcurrency::CCombinedCallbackReference> pCombinedReference = fg_Construct();
			
			bool bOnStateChangeRegistered = false;
			if (Params.m_fOnStateChange)
			{
				bOnStateChangeRegistered = true;
				pCombinedReference->m_References.f_Insert(Internal.m_OnStateChange.f_Register(_CallbackActor, fg_Move(Params.m_fOnStateChange)));
			}
			
			Params.m_fOnStateChange = [this, bOnStateChangeRegistered, pState](CProcessLaunchStateChangeVariant const &_State, fp64 _TimeSinceStart)
				{
					auto ThisActor = pState->m_ThisWeak.f_Lock();
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
			
			if (fp_WillFilterOutput() || Params.m_fOnOutput || (pState->m_ToLog & (ELogFlag_StdOut | ELogFlag_StdErr | ELogFlag_Error)))
			{
				if (Params.m_fOnOutput)
					pCombinedReference->m_References.f_Insert(Internal.m_OnOutput.f_Register(_CallbackActor, fg_Move(Params.m_fOnOutput)));
				
				Params.m_fOnOutput = [pState](EProcessLaunchOutputType _OutputType, NMib::NStr::CStr const &_Output)
					{
						DMibFastCheck(_OutputType < EProcessLaunchOutputType_Max);
						pState->m_OutputBuffers[_OutputType] += _Output;
						pState->f_ProcessOutput(_OutputType, false);
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
				Internal.m_pProcessLaunch = fg_Construct(Params, Launch.m_DestructFlags);
				Continuation.f_SetResult(fg_Move(pCombinedReference));
			}
			catch (NException::CException const &_Exception)
			{
				if (pState->m_ToLog & ELogFlag_Error)
				{
					auto LogScope = pState->f_LogScope(); 
					(void)_Exception;
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
		
		NConcurrency::TCContinuation<void> CProcessLaunchActor::f_SendStdIn(NMib::NStr::CStrSecure const &_Data) const
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

		NConcurrency::TCContinuation<void> CProcessLaunchActor::f_CloseStdIn() const
		{
			auto &Internal = *mp_pInternal;
			return Internal.f_RunBlocking
				(
					[](NPtr::TCSharedPointer<CProcessLaunch> const &_pProcessLaunch)
					{
						_pProcessLaunch->f_CloseStdIn();
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
		
		NStr::CStr CProcessLaunchActor::CSimpleLaunchResult::f_GetStdOut() const
		{
			NStr::CStr Return;
			for (auto &Output : m_Output)
			{
				if (Output.m_Type == EProcessLaunchOutputType_StdOut)
					Return += Output.m_Output;
			}
			return Return;
		}
		
		NStr::CStr CProcessLaunchActor::CSimpleLaunchResult::f_GetErrorOut() const
		{
			NStr::CStr Return;
			for (auto &Output : m_Output)
			{
				if (Output.m_Type != EProcessLaunchOutputType_StdOut)
					Return += Output.m_Output;
			}
			return Return;
		}
		
		NStr::CStr CProcessLaunchActor::CSimpleLaunchResult::f_GetCombinedOut() const
		{
			NStr::CStr Return;
			for (auto &Output : m_Output)
				Return += Output.m_Output;
			return Return;
		}
	}
}

