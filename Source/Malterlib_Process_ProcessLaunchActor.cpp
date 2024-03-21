// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Concurrency/ActorFunctorWeak>
#include <Mib/Concurrency/ActorSubscription>
#include <Mib/Concurrency/ActorSequencerActor>
#include <Mib/Concurrency/LogError>
#include "Malterlib_Process_ProcessLaunchActor.h"

#ifndef DPlatformFamily_Windows
#include <Mib/Core/PlatformSpecific/PosixErrNo>
#include <errno.h>
#include <signal.h>
#endif

namespace NMib::NProcess
{
	static NThread::CMutual g_StdOutLogLock;
	struct CProcessLaunchActor::CInternal : public NConcurrency::CActorInternal
	{
		NStorage::TCSharedPointer<CProcessLaunch> m_pProcessLaunch;
		NConcurrency::TCActorFunctorWeak<NConcurrency::TCFuture<void> (CProcessLaunchStateChangeVariant const &_State, fp64 _TimeSinceStart)> m_fOnStateChange;
		NConcurrency::TCActorFunctorWeak<NConcurrency::TCFuture<void> (EProcessLaunchOutputType _OutputType, NMib::NStr::CStr const &_Output)> m_fOnOutput;

		struct CPendingStop
		{
			NFunction::TCFunctionMovable<void (uint32 _ExitCode)> m_fOnStop;
			NFunction::TCFunctionMovable<void ()> m_fOnException;
			bool m_bStopRun;
		};

		NContainer::TCLinkedList<CPendingStop> m_PendingProcessStops;

		NConcurrency::CSequencer m_SendSequencer{"ProcessLaunchSend"};

		EProcessLaunchCloseFlag m_DestructFlags;

		bool m_bProcessRunning = false;
		bool m_bProcessExited = false;

		NConcurrency::TCFuture<void> f_RunBlocking(NFunction::TCFunctionMovable<void (NStorage::TCSharedPointer<CProcessLaunch> const &_pProcessLaunch)> _fSend);
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

	NConcurrency::TCFuture<void> CProcessLaunchActor::fp_Destroy()
	{
		auto &Internal = *mp_pInternal;
		if (!Internal.m_pProcessLaunch || Internal.m_bProcessExited)
		{
			fg_Move(Internal.m_fOnStateChange).f_Destroy() > NConcurrency::fg_DiscardResult();
			fg_Move(Internal.m_fOnOutput).f_Destroy() > NConcurrency::fg_DiscardResult();
			Internal.m_PendingProcessStops.f_Clear();
			co_return {};
		}

		bool bStopRun = false;
		if (Internal.m_bProcessRunning)
		{
			try
			{
				Internal.m_pProcessLaunch->f_StopProcess();
			}
			catch (NException::CException const &)
			{
				co_return {};
			}
			bStopRun = true;
		}

		auto &Pending = Internal.m_PendingProcessStops.f_Insert();
		NConcurrency::TCPromise<void> PendingStopPromise;
		Pending.m_bStopRun = bStopRun;
		Pending.m_fOnStop = [PendingStopPromise](uint32 _ExitCode)
			{
				PendingStopPromise.f_SetResult();
			}
		;
		Pending.m_fOnException = [PendingStopPromise]()
			{
				PendingStopPromise.f_SetCurrentException();
			}
		;

		co_await PendingStopPromise.f_MoveFuture();

		co_await fg_Move(Internal.m_SendSequencer).f_Destroy().f_Wrap() > NConcurrency::fg_LogError("ProcessLaunch", "Failed to destroy send sequencer");

		co_return {};
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

	NConcurrency::TCFuture<CProcessLaunchActor::CSimpleLaunchResult> CProcessLaunchActor::f_LaunchSimple(CSimpleLaunch const &_SimpleLaunch)
	{
		struct CState
		{
			NConcurrency::TCPromise<CProcessLaunchActor::CSimpleLaunchResult> m_Promise;
			NConcurrency::CActorSubscription m_Subscription;
			CProcessLaunchActor::CSimpleLaunchResult m_LaunchResult;
			ESimpleLaunchFlag m_SimpleFlags = ESimpleLaunchFlag_None;
		};

		NStorage::TCSharedPointer<CState> pState = fg_Construct();

		if (_SimpleLaunch.m_Params.m_fOnOutput)
			return pState->m_Promise <<= DMibErrorInstance("On output cannot be specified for simple launch");
		if (_SimpleLaunch.m_Params.m_fOnStateChange)
			return pState->m_Promise <<= DMibErrorInstance("On state change cannot be specified for simple launch");

		pState->m_SimpleFlags = _SimpleLaunch.m_SimpleFlags;

		CLaunch Params{_SimpleLaunch};
		Params.m_Params.m_fOnStateChange = [pState, bSeparateStdErr = _SimpleLaunch.m_Params.m_bSeparateStdErr](CProcessLaunchStateChangeVariant const &_StateChange, fp64 _TimeSinceLaunch)
			{
				switch (_StateChange.f_GetTypeID())
				{
				case EProcessLaunchState_Launched:
					{
					}
					break;
				case EProcessLaunchState_LaunchFailed:
					{
						pState->m_Promise.f_SetException(DMibErrorInstance(fg_Format("Launch failed: {}", _StateChange.f_Get<EProcessLaunchState_LaunchFailed>())));
						pState->m_Subscription.f_Clear();
					}
					break;
				case EProcessLaunchState_Exited:
					{
						int32 ExitCode = _StateChange.f_Get<EProcessLaunchState_Exited>();
						if (ExitCode && (pState->m_SimpleFlags & ESimpleLaunchFlag_GenerateExceptionOnNonZeroExitCode))
						{
							pState->m_Promise.f_SetException
								(
									DMibErrorInstance
									(
										fg_Format
										(
											"Launch exited with {} (0x{nfh,sj8,sf0}): {}"
											, ExitCode
											, uint32(ExitCode)
											, (bSeparateStdErr ? pState->m_LaunchResult.f_GetErrorOut() : pState->m_LaunchResult.f_GetCombinedOut()).f_Trim()
										)
									)
								)
							;
						}
						else
						{
							pState->m_LaunchResult.m_ExitCode = ExitCode;
							pState->m_Promise.f_SetResult(fg_Move(pState->m_LaunchResult));
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

		f_Launch(Params, fg_ThisActor(this)) > pState->m_Promise / [pState](NConcurrency::CActorSubscription &&_Subscription)
			{
				pState->m_Subscription = fg_Move(_Subscription);
			}
		;

		return pState->m_Promise.f_Future();
	}

	NConcurrency::TCFuture<NConcurrency::CActorSubscription> CProcessLaunchActor::f_Launch
		(
			CLaunch const &_Launch
			, NConcurrency::TCActor<NConcurrency::CActor> &&_CallbackActor
		)
	{
		NConcurrency::TCPromise<NConcurrency::CActorSubscription> Promise;

		DMibRequire(!mp_pInternal);

		mp_pInternal = fg_Construct();
		auto &Internal = *mp_pInternal;
		Internal.m_DestructFlags = _Launch.m_DestructFlags;

		CLaunch Launch = _Launch;
		fp_ModifyLaunch(Launch);

		struct CState
		{
#if (DMibSysLogSeverities) != 0
			static NMib::NLog::CSysLogCatScope fs_LogScope(NStr::CStr const &_LogName)
			{
				return NMib::NLog::CSysLogCatScope{NMib::fg_GetSys()->f_GetLogger(), _LogName};
			}

			NMib::NLog::CSysLogCatScope f_LogScope() const
			{
				return NMib::NLog::CSysLogCatScope{NMib::fg_GetSys()->f_GetLogger(), m_LogName};
			}
#endif

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
				{
					Output = fg_Move(m_OutputBuffers[_OutputType]);
					m_OutputBuffersParsedChars[_OutputType] = 0;
				}
				else
				{
					auto &OutputBuffer = m_OutputBuffers[_OutputType];
					auto *pParse = OutputBuffer.f_GetStr();
					auto *pFinishedOutput = pParse;
					auto *pStartParse = pParse;
					pParse += m_OutputBuffersParsedChars[_OutputType];
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
					mint nFinishedChars = pFinishedOutput - pStartParse;
					if (!nFinishedChars)
					{
						m_OutputBuffersParsedChars[_OutputType] = pParse - pStartParse;
						return;
					}
					Output = OutputBuffer.f_Extract(0, nFinishedChars);
					fg_StrDelete(OutputBuffer, 0, nFinishedChars);
					m_OutputBuffersParsedChars[_OutputType] = 0;
				}

				if (Output.f_IsEmpty())
					return;

				auto ThisActor = m_ThisWeak.f_Lock();
				if (!ThisActor)
					return; // Already deleted

				NConcurrency::g_Dispatch(ThisActor) / [pThis = m_pThis, _OutputType, Output = fg_Move(Output), ToLog = m_ToLog, LogName = m_LogName]() mutable
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
#if (DMibSysLogSeverities) != 0
									auto LogScope = fs_LogScope(LogName);
									DMibLogOperation(StdOut);
									DMibLog(Info, "{}", Output.f_TrimRight());
#endif
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
#if (DMibSysLogSeverities) != 0
									auto LogScope = fs_LogScope(LogName);
									DMibLogOperation(StdErr);
									DMibLog(Error, "{}", Output.f_TrimRight());
#endif
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
#if (DMibSysLogSeverities) != 0
									auto LogScope = fs_LogScope(LogName);
									DMibLog(Error, "{}", Output.f_TrimRight());
#endif
								}
								break;
							}
						}

						if (Internal.m_fOnOutput)
							Internal.m_fOnOutput(_OutputType, Output) > NConcurrency::fg_DiscardResult();
					}
					> NConcurrency::fg_DiscardResult()
				;
			}

			NStorage::CIntrusiveRefCount m_RefCount;
			ELogFlag m_ToLog = ELogFlag_None;
			NStr::CStr m_LogName;
			NStr::CStr m_OutputBuffers[EProcessLaunchOutputType_Max];
			mint m_OutputBuffersParsedChars[EProcessLaunchOutputType_Max] = {};
			NConcurrency::TCWeakActor<CProcessLaunchActor> m_ThisWeak;
			CProcessLaunchActor *m_pThis = nullptr;
			bool m_bWholeLineOutput = true;
		};

		NStorage::TCSharedPointer<CState> pState = fg_Construct();

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

		if (Params.m_fOnStateChange)
		{
			Internal.m_fOnStateChange = NConcurrency::g_ActorFunctorWeak(_CallbackActor) / [fOnStateChange = fg_Move(Params.m_fOnStateChange)]
				(CProcessLaunchStateChangeVariant const &_State, fp64 _TimeSinceStart)
				-> NConcurrency::TCFuture<void>
				{
					fOnStateChange(_State, _TimeSinceStart);

					co_return {};
				}
			;
		}

		Params.m_fOnStateChange = [this, pState](CProcessLaunchStateChangeVariant const &_State, fp64 _TimeSinceStart)
			{
				auto ThisActor = pState->m_ThisWeak.f_Lock();
				if (!ThisActor)
					return; // Already deleted

				auto LaunchState = _State.f_GetTypeID();
				if (LaunchState == NProcess::EProcessLaunchState_Exited || LaunchState == NProcess::EProcessLaunchState_LaunchFailed)
					pState->f_FlushOutput();

				ThisActor
					(
						&CActor::f_Dispatch
						, NFunction::TCFunctionMovable<void ()>
						(
							[this, _TimeSinceStart, State = _State, pState]
							{
								auto &Internal = *mp_pInternal;

								if (Internal.m_fOnStateChange)
									Internal.m_fOnStateChange(State, _TimeSinceStart) > NConcurrency::fg_DiscardResult();

								switch (State.f_GetTypeID())
								{
								case NProcess::EProcessLaunchState_Exited:
									{
										uint32 ExitCode = State.f_Get<NProcess::EProcessLaunchState_Exited>();

#if (DMibSysLogSeverities) != 0
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
#endif

										for (auto &Pending : Internal.m_PendingProcessStops)
											Pending.m_fOnStop(ExitCode);

										Internal.m_PendingProcessStops.f_Clear();
										Internal.m_bProcessExited = true;
									}
									break;
								case NProcess::EProcessLaunchState_Launched:
									{
#if (DMibSysLogSeverities) != 0
										if (pState->m_ToLog & ELogFlag_Info)
										{
											auto LogScope = pState->f_LogScope();
											DMibLog(Info, "Launched");
										}
#endif
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
#if (DMibSysLogSeverities) != 0
										if (pState->m_ToLog & ELogFlag_Error)
										{
											auto LogScope = pState->f_LogScope();
											DMibLog(Error, "Launch failed: {}", State.f_Get<EProcessLaunchState_LaunchFailed>());
										}
#endif
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
			{
				Internal.m_fOnOutput = NConcurrency::g_ActorFunctorWeak(_CallbackActor) / [fOnOutput = fg_Move(Params.m_fOnOutput)]
					(EProcessLaunchOutputType _OutputType, NMib::NStr::CStr const &_Output)
					-> NConcurrency::TCFuture<void>
					{
						fOnOutput(_OutputType, _Output);

						co_return {};
					}
				;
			}

			Params.m_fOnOutput = [pState](EProcessLaunchOutputType _OutputType, NMib::NStr::CStr const &_Output)
				{
					DMibFastCheck(_OutputType < EProcessLaunchOutputType_Max);
					pState->m_OutputBuffers[_OutputType] += _Output;
					pState->f_ProcessOutput(_OutputType, false);
				}
			;
		}

#if (DMibSysLogSeverities) != 0
		if (pState->m_ToLog & ELogFlag_Info)
		{
			auto LogScope = pState->f_LogScope();
			DMibLog(Info, "Launching");
		}
#endif

		try
		{
			Internal.m_pProcessLaunch = fg_Construct(Params, Launch.m_DestructFlags);
			Promise.f_SetResult
				(
					NConcurrency::g_ActorSubscription / [this]() -> NConcurrency::TCFuture<void>
					{
						auto &Internal = *mp_pInternal;
						NConcurrency::TCActorResultVector<void> Results;

						if (!Internal.m_fOnOutput.f_IsEmpty())
							fg_Move(Internal.m_fOnOutput).f_Destroy() > Results.f_AddResult();

						if (!Internal.m_fOnStateChange.f_IsEmpty())
							fg_Move(Internal.m_fOnStateChange).f_Destroy() > Results.f_AddResult();

						co_await Results.f_GetResults();

						co_return {};
					}
				)
			;
		}
		catch (NException::CException const &_Exception)
		{
			(void)_Exception;
#if (DMibSysLogSeverities) != 0
			if (pState->m_ToLog & ELogFlag_Error)
			{
				auto LogScope = pState->f_LogScope();
				DMibLog(Error, "Exception launching: {}", _Exception.f_GetErrorStr());
			}
#endif
			Promise.f_SetCurrentException();
		}
		catch (...)
		{
			Promise.f_SetCurrentException();
		}

		return Promise.f_MoveFuture();
	}

	NConcurrency::TCFuture<void> CProcessLaunchActor::CInternal::f_RunBlocking(NFunction::TCFunctionMovable<void (NStorage::TCSharedPointer<CProcessLaunch> const &_pProcessLaunch)> _fSend)
	{
		if (!m_pProcessLaunch)
			co_return {};

		auto SequencerSubscription = co_await m_SendSequencer.f_Sequence();

		auto BlockingActorCheckout = NConcurrency::fg_BlockingActor();

		// Dispatch on separate thread actor as this can block
		co_await
			(
				NConcurrency::g_Dispatch(BlockingActorCheckout) / [fSend = fg_Move(_fSend), pProcessLaunch = m_pProcessLaunch]() mutable
				{
					fSend(pProcessLaunch);
				}
			)
		;

		co_return {};
	}

	NConcurrency::TCFuture<void> CProcessLaunchActor::f_SendStdInBinary(NContainer::CSecureByteVector const &_Data) const
	{
		auto &Internal = *mp_pInternal;
		return Internal.f_RunBlocking
			(
				[_Data](NStorage::TCSharedPointer<CProcessLaunch> const &_pProcessLaunch)
				{
					_pProcessLaunch->f_SendStdInBinary(_Data);
				}
			)
		;
	}

	NConcurrency::TCFuture<void> CProcessLaunchActor::f_SendStdIn(NMib::NStr::CStrSecure const &_Data) const
	{
		auto &Internal = *mp_pInternal;
		return Internal.f_RunBlocking
			(
				[_Data](NStorage::TCSharedPointer<CProcessLaunch> const &_pProcessLaunch)
				{
					_pProcessLaunch->f_SendStdIn(_Data);
				}
			)
		;
	}

	NConcurrency::TCFuture<void> CProcessLaunchActor::f_CloseStdIn() const
	{
		auto &Internal = *mp_pInternal;
		return Internal.f_RunBlocking
			(
				[](NStorage::TCSharedPointer<CProcessLaunch> const &_pProcessLaunch)
				{
					_pProcessLaunch->f_CloseStdIn();
				}
			)
		;
	}

	NConcurrency::TCFuture<void> CProcessLaunchActor::f_Signal(int32 _Signal) const
	{
		NConcurrency::TCPromise<uint32> Promise;

		auto &Internal = *mp_pInternal;
		if (!Internal.m_pProcessLaunch)
			co_return {};

		if (Internal.m_bProcessRunning)
		{
			auto ProcessId = Internal.m_pProcessLaunch->f_GetProcessID();

#ifdef DPlatformFamily_Windows
			co_return DMibErrorInstance("Signal is not supported on Windows");
#else
			if (kill(ProcessId, _Signal))
				co_return DMibErrorInstance(NMib::NPlatform::fg_FormatErrno(NMib::NStr::CStr::CFormat("kill({}, {})") << ProcessId << _Signal, errno));
#endif
		}

		co_return {};
	}

	NConcurrency::TCFuture<uint32> CProcessLaunchActor::fp_StopProcess(bool _bGroup) const
	{
		NConcurrency::TCPromise<uint32> Promise;

		auto &Internal = *mp_pInternal;
		if (!Internal.m_pProcessLaunch)
			return Promise <<= 0;

		bool bStopRun = false;

		if (Internal.m_bProcessRunning)
		{
			try
			{
				if (_bGroup)
					Internal.m_pProcessLaunch->f_StopProcessGroup();
				else
					Internal.m_pProcessLaunch->f_StopProcess();
			}
			catch (NException::CException const &)
			{
				return Promise <<= NException::fg_CurrentException();
			}
			bStopRun = true;
		}
		auto &Pending = Internal.m_PendingProcessStops.f_Insert();
		Pending.m_bStopRun = bStopRun;
		Pending.m_fOnStop = [Promise](uint32 _ExitCode)
			{
				Promise.f_SetResult(_ExitCode);
			}
		;
		Pending.m_fOnException = [Promise]()
			{
				Promise.f_SetCurrentException();
			}
		;
		return Promise.f_MoveFuture();
	}

	NConcurrency::TCFuture<uint32> CProcessLaunchActor::f_StopProcess() const
	{
		return fp_StopProcess(false);
	}

	NConcurrency::TCFuture<uint32> CProcessLaunchActor::f_StopProcessGroup() const
	{
		return fp_StopProcess(true);
	}

	template <typename tf_CType, typename tf_FToWrap>
	NConcurrency::TCFuture<tf_CType> fg_WrapFuture(tf_FToWrap &&_fToWrap)
	{
		NConcurrency::TCPromise<tf_CType> Promise;
		try
		{
			Promise.f_SetResult(_fToWrap());
		}
		catch (...)
		{
			Promise.f_SetCurrentException();
		}
		return Promise.f_MoveFuture();
	}

	NConcurrency::TCFuture<fp64> CProcessLaunchActor::f_GetRunningTime() const
	{
		return fg_WrapFuture<fp64>
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

	NConcurrency::TCFuture<CProcessStatistics> CProcessLaunchActor::f_GetExecutionStatistics() const
	{
		return fg_WrapFuture<CProcessStatistics>
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

	NConcurrency::TCFuture<CProcessStatistics> CProcessLaunchActor::f_GetMemoryStatistics() const
	{
		return fg_WrapFuture<CProcessStatistics>
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

	NConcurrency::TCFuture<CProcessStatistics> CProcessLaunchActor::f_GetOverallExecutionStatistics() const
	{
		return fg_WrapFuture<CProcessStatistics>
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

	NConcurrency::TCFuture<CProcessStatistics> CProcessLaunchActor::f_GetOverallMemoryStatistics() const
	{
		return fg_WrapFuture<CProcessStatistics>
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

	NStr::CStr CProcessLaunchActor::CSimpleLaunchResult::f_GetStdErr() const
	{
		NStr::CStr Return;
		for (auto &Output : m_Output)
		{
			if (Output.m_Type == EProcessLaunchOutputType_StdErr)
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

	auto DMibWorkaroundUBSanSectionErrors CProcessLaunchActor::fs_LaunchSimple(CSimpleLaunch _SimpleLaunch) -> NConcurrency::TCFuture<CSimpleLaunchResult>
	{
		NConcurrency::TCActor<CProcessLaunchActor> LaunchActor;
		LaunchActor = fg_Construct();
		co_return co_await LaunchActor(&CProcessLaunchActor::f_LaunchSimple, fg_Move(_SimpleLaunch));
	}
}
