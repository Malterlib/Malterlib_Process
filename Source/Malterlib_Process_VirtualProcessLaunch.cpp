// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Core/Core>
#include <Mib/Process/VirtualProcessLaunch>

namespace NMib::NProcess
{
	CVirtualProcessLaunch::~CVirtualProcessLaunch()
	{
	}

	// Stops the process group of a launch made with m_bCreateNewProcessGroup. A launch without process groups stops the process
	void CVirtualProcessLaunch::f_StopProcessGroup() const
	{
		f_StopProcess();
	}

	bool CProcessLaunchHandler::CLaunchInfo::fp_Lingering() const
	{
		if (mp_pProcessLaunch)
		{
			if (mp_pProcessLaunch->f_IsOpen())
				return true;
			return (mp_pProcessLaunch->f_GetCloseFlags() & EProcessLaunchCloseFlag_LingerUntilDone) != 0;
		}
		return false;
	}

	CProcessLaunchHandler::CLaunchInfo::CLaunchInfo(CProcessLaunchParams const &_Params, FVirtualProcessLaunchFactory const &_fFactory)
		: mp_LaunchParams(_Params)
		, mp_Completion(ECompletion::mc_Running)
		, mp_fFactory(_fFactory)
	{
	}

	CVirtualProcessLaunch *CProcessLaunchHandler::CLaunchInfo::f_GetProcessLaunch() const
	{
		return mp_pProcessLaunch.f_Get();
	}

	CProcessLaunchHandler::CProcessLaunchHandler()
	{
		mp_pDestroyedNotification = fg_Construct();
	}

	CProcessLaunchHandler::~CProcessLaunchHandler()
	{
		{
			DMibLock(mp_pDestroyedNotification->m_DestroyLock);
			mp_pDestroyedNotification->m_Destroyed.f_Exchange(1);
		}
		mp_pDestroyedNotification = nullptr;
		for (auto iLaunch = mp_Launches.f_GetIterator(); iLaunch; ++iLaunch)
		{
			if (iLaunch->mp_pProcessLaunch && iLaunch->mp_pProcessLaunch->f_IsOpen())
			{
				iLaunch->mp_Completion.f_Store(CLaunchInfo::ECompletion::mc_Reported);
				iLaunch->mp_pProcessLaunch->f_Close(EProcessLaunchCloseFlag_None);
			}
		}
	}

	void CProcessLaunchHandler::CLaunchInfo::fp_Clear()
	{
		mp_LaunchParams.m_fDispatcher.f_Clear();
		mp_LaunchParams.m_fOnOutput.f_Clear();
		mp_LaunchParams.m_fOnOutputBinary.f_Clear();
		mp_LaunchParams.m_fOnStateChange.f_Clear();
	}

	CProcessLaunchHandler::CLaunchInfo *CProcessLaunchHandler::f_AddLaunch(CProcessLaunchParams const &_Params, bool _bDelayOutput, FVirtualProcessLaunchFactory const &_fLaunchFactory)
	{
		auto pInfo = &mp_Launches.f_Insert(fg_Construct(_Params, _fLaunchFactory));

		CProcessLaunchParams Params = _Params;

		NStorage::TCSharedPointer<CDestroyed> pDestroyedNotification = mp_pDestroyedNotification;

		Params.m_fOnStateChange = [_bDelayOutput, pDestroyedNotification, this, pInfo](CProcessLaunchStateChangeVariant const &_StateChange, fp64 _TimeSinceLaunch)
			{
				NFunction::TCFunction<void (CProcessLaunchStateChangeVariant const &_State, fp64 _TimeSinceStart)> fOnStateChange;
				{
					DMibLock(pDestroyedNotification->m_DestroyLock);

					if (pDestroyedNotification->m_Destroyed.f_Load())
						return; // No longer valid

					if (_StateChange.f_GetTypeID() == EProcessLaunchState_Exited)
					{
						if (_bDelayOutput)
						{
							DMibLock(pInfo->mp_DelayedOutputLock);
							for (auto &Delayed : pInfo->mp_DelayedOutput)
							{
								if (Delayed.m_bBinary)
									pInfo->mp_LaunchParams.m_fOnOutputBinary(Delayed.m_Type, Delayed.m_BinaryOutput);
								else
									pInfo->mp_LaunchParams.m_fOnOutput(Delayed.m_Type, Delayed.m_Output);
							}
						}
					}

					fOnStateChange = pInfo->mp_LaunchParams.m_fOnStateChange;

					if (_StateChange.f_GetTypeID() == EProcessLaunchState_LaunchFailed)
					{
						pInfo->mp_Completion.f_Store(CLaunchInfo::ECompletion::mc_Reported);
						pInfo->fp_Clear();
					}
					else if (_StateChange.f_GetTypeID() == EProcessLaunchState_Exited)
					{
						pInfo->mp_Completion.f_Store(CLaunchInfo::ECompletion::mc_Reported);
						pInfo->fp_Clear();
					}
				}
				if (fOnStateChange)
					fOnStateChange(_StateChange, _TimeSinceLaunch);

				if (_StateChange.f_GetTypeID() == EProcessLaunchState_Exited || _StateChange.f_GetTypeID() == EProcessLaunchState_LaunchFailed)
					pInfo->mp_Completion.f_Store(CLaunchInfo::ECompletion::mc_CallbackFinished);

				mp_LaunchChanged.f_Signal();
			}
		;

		if (_bDelayOutput && _Params.m_fOnOutput)
		{
			Params.m_fOnOutput = [pDestroyedNotification, pInfo](EProcessLaunchOutputType _OutputType, NStr::CStr const &_Output)
				{
					DMibLock(pDestroyedNotification->m_DestroyLock);
					if (pDestroyedNotification->m_Destroyed.f_Load())
						return; // No longer valid

					{
						DMibLock(pInfo->mp_DelayedOutputLock);
						auto &Output = pInfo->mp_DelayedOutput.f_Insert();
						Output.m_Type = _OutputType;
						Output.m_Output = _Output;
					}
				}
			;
		}

		if (_bDelayOutput && _Params.m_fOnOutputBinary)
		{
			Params.m_fOnOutputBinary = [pDestroyedNotification, pInfo](EProcessLaunchOutputType _OutputType, NContainer::CIOByteVector const &_Output)
				{
					DMibLock(pDestroyedNotification->m_DestroyLock);
					if (pDestroyedNotification->m_Destroyed.f_Load())
						return; // No longer valid

					{
						DMibLock(pInfo->mp_DelayedOutputLock);
						auto &Output = pInfo->mp_DelayedOutput.f_Insert();
						Output.m_Type = _OutputType;
						Output.m_BinaryOutput = _Output;
						Output.m_bBinary = true;
					}
				}
			;
		}

		if (pInfo->mp_fFactory)
			pInfo->mp_pProcessLaunch = pInfo->mp_fFactory(Params, EProcessLaunchCloseFlag_None);
		else
			pInfo->mp_pProcessLaunch = fg_Construct<CVirtualProcessLaunch_Default>(Params, EProcessLaunchCloseFlag_None);

		return pInfo;
	}

	// _Termination is EProcessLaunchCloseFlag_TerminateProcess or EProcessLaunchCloseFlag_TerminateProcessTree
	void CProcessLaunchHandler::f_TerminateAll(bool _bBlock, NContainer::TCVector<CProcessStatistics> *o_pMemoryStats, EProcessLaunchCloseFlag _Termination)
	{
		for (auto iLaunch = mp_Launches.f_GetIterator(); iLaunch; ++iLaunch)
		{
			if (iLaunch->mp_pProcessLaunch && iLaunch->mp_pProcessLaunch->f_IsOpen())
			{
				if (o_pMemoryStats)
					o_pMemoryStats->f_Insert(iLaunch->mp_pProcessLaunch->f_GetOverallMemoryStatistics());

				iLaunch->mp_pProcessLaunch->f_Close(_Termination | (_bBlock ? EProcessLaunchCloseFlag_BlockOnExit : EProcessLaunchCloseFlag_LingerUntilDone));
			}
		}
	}

	// _bProcessGroups also stops what the launches started, for launches made with m_bCreateNewProcessGroup
	void CProcessLaunchHandler::f_StopAll(bool _bProcessGroups)
	{
		NException::CDisableExceptionTraceScope DisableExceptionTrace;
		for (auto &Launch : mp_Launches)
		{
			if (!Launch.mp_pProcessLaunch || !Launch.mp_pProcessLaunch->f_IsOpen())
				continue;
			try
			{
				if (_bProcessGroups)
					Launch.mp_pProcessLaunch->f_StopProcessGroup();
				else
					Launch.mp_pProcessLaunch->f_StopProcess();
			}
			catch (NException::CException const &)
			{
			}
		}
	}

	// Serialize with other handler operations. With a dispatcher, call on its thread outside callbacks, after dispatch has completed.
	// Without a dispatcher, call outside launch callbacks; open launches are joined before removal. Any already closed launch must have been joined.
	// Removing a launch invalidates its CLaunchInfo and CVirtualProcessLaunch pointers.
	void CProcessLaunchHandler::f_ReapCompleted()
	{
		for (auto iLaunch = mp_Launches.f_GetIterator(); iLaunch;)
		{
			auto *pInfo = &*iLaunch;
			++iLaunch;

			if (pInfo->mp_Completion.f_Load() != CLaunchInfo::ECompletion::mc_CallbackFinished)
				continue;

			if (pInfo->mp_pProcessLaunch && pInfo->mp_pProcessLaunch->f_IsOpen())
				pInfo->mp_pProcessLaunch->f_Close(EProcessLaunchCloseFlag_BlockOnExit);

			mp_Launches.f_Remove(*pInfo);
		}
	}

	bool CProcessLaunchHandler::f_WaitForChange(fp32 _Timeout)
	{
		if (_Timeout == 0.0)
		{
			mp_LaunchChanged.f_Wait();
			return false;
		}
		else
			return mp_LaunchChanged.f_WaitTimeout(_Timeout);
	}

	bool CProcessLaunchHandler::f_BlockOnExit(fp32 _Timeout, umint _nMaxRunning, NContainer::TCVector<CProcessStatistics> *o_pMemoryStats)
	{
		NTime::CStopwatch BlockTime;
		BlockTime.f_Start();
		while (1)
		{
			umint nRunning = 0;
			for (auto iLaunch = mp_Launches.f_GetIterator(); iLaunch; ++iLaunch)
			{
				if (iLaunch->mp_Completion.f_Load() == CLaunchInfo::ECompletion::mc_Running && iLaunch->fp_Lingering())
				{
					++nRunning;
					if (nRunning > _nMaxRunning)
						break;
				}
			}
			if (nRunning <= _nMaxRunning)
			{
				for (auto iLaunch = mp_Launches.f_GetIterator(); iLaunch; ++iLaunch)
				{
					if (iLaunch->mp_Completion.f_Load() != CLaunchInfo::ECompletion::mc_Running || !iLaunch->fp_Lingering())
					{
						if (iLaunch->mp_pProcessLaunch->f_IsOpen())
						{
							if (o_pMemoryStats)
								o_pMemoryStats->f_Insert(iLaunch->mp_pProcessLaunch->f_GetOverallMemoryStatistics());

							iLaunch->mp_pProcessLaunch->f_Close(EProcessLaunchCloseFlag_BlockOnExit);
						}
					}
				}
				return true;
			}
			if (_Timeout != 0.0 && BlockTime.f_GetTime() > _Timeout)
				return false;

			mp_LaunchChanged.f_WaitTimeout(_Timeout != 0.0 ? _Timeout / 10.0 : 10.0);
		}
	}

	CProcessLaunchHandler::CLaunchInfo *CProcessLaunchHandler::f_GetFirstNotDone()
	{
		for (auto iLaunch = mp_Launches.f_GetIterator(); iLaunch; ++iLaunch)
		{
			if (iLaunch->mp_Completion.f_Load() == CLaunchInfo::ECompletion::mc_Running && iLaunch->fp_Lingering())
				return iLaunch;
		}
		return nullptr;
	}

	CVirtualProcessLaunch_Default::CVirtualProcessLaunch_Default(CProcessLaunchParams const &_Params, EProcessLaunchCloseFlag _DestructFlags)
		: mp_Launch(_Params, _DestructFlags)
		, mp_bCreatedProcessGroup(_Params.m_bCreateNewProcessGroup)
	{
	}

	CVirtualProcessLaunch_Default::~CVirtualProcessLaunch_Default()
	{
	}

	CVirtualProcessLaunch_Default::CVirtualProcessLaunch_Default(CVirtualProcessLaunch_Default &&_Other)
		: mp_Launch(fg_Move(_Other.mp_Launch))
		, mp_bCreatedProcessGroup(_Other.mp_bCreatedProcessGroup)
	{
	}

	EProcessLaunchCloseFlag CVirtualProcessLaunch_Default::f_GetCloseFlags() const
	{
		return mp_Launch.f_GetCloseFlags();
	}

	void CVirtualProcessLaunch_Default::f_Close(EProcessLaunchCloseFlag _CloseFlags)
	{
		return mp_Launch.f_Close(_CloseFlags);
	}

	void CVirtualProcessLaunch_Default::f_StopProcess() const
	{
		return mp_Launch.f_StopProcess();
	}

	void CVirtualProcessLaunch_Default::f_StopProcessGroup() const
	{
		if (!mp_bCreatedProcessGroup)
			return mp_Launch.f_StopProcess();

		return mp_Launch.f_StopProcessGroup();
	}

	bool CVirtualProcessLaunch_Default::f_IsOpen() const
	{
		return mp_Launch.f_IsOpen();
	}

	bool CVirtualProcessLaunch_Default::f_IsRunning() const
	{
		return mp_Launch.f_IsRunning();
	}

	void CVirtualProcessLaunch_Default::f_SendStdIn(NMib::NStr::CStrIO const &_Data) const
	{
		return mp_Launch.f_SendStdIn(_Data);
	}

	void CVirtualProcessLaunch_Default::f_CloseStdIn() const
	{
		return mp_Launch.f_CloseStdIn();
	}

	fp64 CVirtualProcessLaunch_Default::f_GetRunningTime() const
	{
		return mp_Launch.f_GetRunningTime();
	}

	umint CVirtualProcessLaunch_Default::f_GetProcessID() const
	{
		return mp_Launch.f_GetProcessID();
	}

	CProcessStatistics CVirtualProcessLaunch_Default::f_GetExecutionStatistics() const
	{
		return mp_Launch.f_GetExecutionStatistics();
	}

	CProcessStatistics CVirtualProcessLaunch_Default::f_GetMemoryStatistics() const
	{
		return mp_Launch.f_GetMemoryStatistics();
	}

	CProcessStatistics CVirtualProcessLaunch_Default::f_GetOverallExecutionStatistics() const
	{
		return mp_Launch.f_GetOverallExecutionStatistics();
	}

	CProcessStatistics CVirtualProcessLaunch_Default::f_GetOverallMemoryStatistics() const
	{
		return mp_Launch.f_GetOverallMemoryStatistics();
	}
}
