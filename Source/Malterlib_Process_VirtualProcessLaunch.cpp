// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Process/VirtualProcessLaunch>

namespace NMib
{
	namespace NProcess
	{

		CVirtualProcessLaunch::~CVirtualProcessLaunch()
		{
		}
		
		bool CProcessLaunchHandler::CLaunchInfo::fp_Lingering() const
		{
			if (m_pProcessLaunch)
			{
				if (m_pProcessLaunch->f_IsOpen())
					return true;
				return (m_pProcessLaunch->f_GetCloseFlags() & EProcessLaunchCloseFlag_LingerUntilDone) != 0;
			}
			return false;
		}

		CProcessLaunchHandler::CLaunchInfo::CLaunchInfo(CProcessLaunchParams const &_Params, FVirtualProcessLaunchFactory const &_Factory)
			: m_LaunchParams(_Params)
			, m_Done(0)
			, m_Factory(_Factory)
		{
		}

		CVirtualProcessLaunch *CProcessLaunchHandler::CLaunchInfo::f_GetProcessLaunch() const
		{
			return m_pProcessLaunch.f_Get();
		}

		CProcessLaunchHandler::CProcessLaunchHandler()
		{
			m_pDestroyedNotification = fg_Construct();
		}

		CProcessLaunchHandler::~CProcessLaunchHandler()
		{
			{
				DMibLock(m_pDestroyedNotification->m_DestroyLock);
				m_pDestroyedNotification->m_Destroyed.f_Exchange(1);
			}
			m_pDestroyedNotification = nullptr;
			for (auto Iter = m_Launches.f_GetIterator(); Iter; ++Iter)
			{
				if (Iter->m_pProcessLaunch && Iter->m_pProcessLaunch->f_IsOpen())
				{
					Iter->m_Done.f_Exchange(1);
					Iter->m_pProcessLaunch->f_Close(EProcessLaunchCloseFlag_None);
				}
			}
		}

		void CProcessLaunchHandler::CLaunchInfo::fp_Clear()
		{
			m_LaunchParams.m_fDispatcher.f_Clear();
			m_LaunchParams.m_fOnOutput.f_Clear();
			m_LaunchParams.m_fOnStateChange.f_Clear();
		}
		CProcessLaunchHandler::CLaunchInfo *CProcessLaunchHandler::f_AddLaunch(CProcessLaunchParams const &_Params, bint _bDelayOutput, FVirtualProcessLaunchFactory const &_LaunchFactory)
		{
			auto pInfo = &m_Launches.f_Insert(fg_Construct(_Params, _LaunchFactory));

			CProcessLaunchParams Params = _Params;

			NPtr::TCSharedPointer<CDestroyed> pDestroyedNotification = m_pDestroyedNotification;

			Params.m_fOnStateChange
				= [_bDelayOutput, pDestroyedNotification, this, pInfo](CProcessLaunchStateChangeVariant const &_StateChange, fp64 _TimeSinceLaunch)
				{
					DMibLock(pDestroyedNotification->m_DestroyLock);

					if (pDestroyedNotification->m_Destroyed.f_Load())
						return; // No longer valid

					if (_StateChange.f_GetTypeID() == EProcessLaunchState_Exited)
					{
						if (_bDelayOutput && pInfo->m_LaunchParams.m_fOnOutput)
						{
							DMibLock(pInfo->m_DelayedOutputLock);
							for (auto &Delayed : pInfo->m_DelayedOutput)
								pInfo->m_LaunchParams.m_fOnOutput(Delayed.m_Type, Delayed.m_Output);
						}
					}

					if (pInfo->m_LaunchParams.m_fOnStateChange)
						pInfo->m_LaunchParams.m_fOnStateChange(_StateChange, _TimeSinceLaunch);

					if (_StateChange.f_GetTypeID() == EProcessLaunchState_LaunchFailed)
					{
						pInfo->m_Done.f_Exchange(1);
						pInfo->fp_Clear();
					}
					else if (_StateChange.f_GetTypeID() == EProcessLaunchState_Exited)
					{
						pInfo->m_Done.f_Exchange(1);
						pInfo->fp_Clear();
					}
					m_LaunchChanged.f_Signal();
				}
			;

			if (_bDelayOutput && _Params.m_fOnOutput)
			{
				Params.m_fOnOutput 
					= [pDestroyedNotification, pInfo](EProcessLaunchOutputType _OutputType, NStr::CStr const &_Output)
					{
						DMibLock(pDestroyedNotification->m_DestroyLock);
						if (pDestroyedNotification->m_Destroyed.f_Load())
							return; // No longer valid

						{
							DMibLock(pInfo->m_DelayedOutputLock);
							auto &Output = pInfo->m_DelayedOutput.f_Insert();
							Output.m_Type = _OutputType;
							Output.m_Output = _Output;
						}
					}
				;
			}

			if (pInfo->m_Factory)
				pInfo->m_pProcessLaunch = pInfo->m_Factory(Params, EProcessLaunchCloseFlag_None);
			else
				pInfo->m_pProcessLaunch = fg_Construct<CVirtualProcessLaunch_Default>(Params, EProcessLaunchCloseFlag_None);
				

			return pInfo;
		}

		void CProcessLaunchHandler::f_TerminateAll()
		{
			for (auto Iter = m_Launches.f_GetIterator(); Iter; ++Iter)
			{
				if (Iter->m_pProcessLaunch && Iter->m_pProcessLaunch->f_IsOpen())
					Iter->m_pProcessLaunch->f_Close(EProcessLaunchCloseFlag_TerminateProcess | EProcessLaunchCloseFlag_LingerUntilDone);
			}
		}

		bint CProcessLaunchHandler::f_WaitForChange(fp32 _Timeout)
		{
			if (_Timeout == 0.0)
			{
				m_LaunchChanged.f_Wait();
				return false;
			}
			else
				return m_LaunchChanged.f_WaitTimeout(_Timeout);
		}

		bint CProcessLaunchHandler::f_BlockOnExit(fp32 _Timeout)
		{
			NTime::CClock BlockTime;
			BlockTime.f_Start();
			while (1)
			{
				bint bDone = true;
				for (auto Iter = m_Launches.f_GetIterator(); Iter; ++Iter)
				{
					if (!Iter->m_Done.f_Load() && Iter->fp_Lingering())
					{
						bDone = false;
						break;
					}
				}
				if (bDone)
				{
					for (auto Iter = m_Launches.f_GetIterator(); Iter; ++Iter)
					{
						if (Iter->m_pProcessLaunch->f_IsOpen())
							Iter->m_pProcessLaunch->f_Close(EProcessLaunchCloseFlag_BlockOnExit);
					}
					return true;;
				}
				if (_Timeout != 0.0 && BlockTime.f_GetTime() > _Timeout)
					return false;

				m_LaunchChanged.f_WaitTimeout(10.0);
			}
		}

		CProcessLaunchHandler::CLaunchInfo *CProcessLaunchHandler::f_GetFirstNotDone()
		{
			for (auto Iter = m_Launches.f_GetIterator(); Iter; ++Iter)
			{
				if (!Iter->m_Done.f_Load() && Iter->fp_Lingering())
					return Iter;
			}
			return nullptr;
		}

		CVirtualProcessLaunch_Default::CVirtualProcessLaunch_Default(CProcessLaunchParams const &_Params, EProcessLaunchCloseFlag _DestructFlags)
			: m_Launch(_Params, _DestructFlags)
		{
		}

		CVirtualProcessLaunch_Default::~CVirtualProcessLaunch_Default()
		{
		}

		CVirtualProcessLaunch_Default::CVirtualProcessLaunch_Default(CVirtualProcessLaunch_Default &&_Other)
			: m_Launch(fg_Move(_Other.m_Launch))
		{
		}


		EProcessLaunchCloseFlag CVirtualProcessLaunch_Default::f_GetCloseFlags() const
		{
			return m_Launch.f_GetCloseFlags();
		}

		void CVirtualProcessLaunch_Default::f_Close(EProcessLaunchCloseFlag _CloseFlags)
		{
			return m_Launch.f_Close(_CloseFlags);
		}

		bint CVirtualProcessLaunch_Default::f_IsOpen() const
		{
			return m_Launch.f_IsOpen();
		}

		bint CVirtualProcessLaunch_Default::f_IsRunning() const
		{
			return m_Launch.f_IsRunning();
		}

		void CVirtualProcessLaunch_Default::f_SendStdIn(NMib::NStr::CStrSecure const &_Data) const
		{
			return m_Launch.f_SendStdIn(_Data);
		}

		fp64 CVirtualProcessLaunch_Default::f_GetRunningTime() const
		{
			return m_Launch.f_GetRunningTime();
		}
		
		CProcessStatistics CVirtualProcessLaunch_Default::f_GetExecutionStatistics() const
		{
			return m_Launch.f_GetExecutionStatistics();
		}
		
		CProcessStatistics CVirtualProcessLaunch_Default::f_GetMemoryStatistics() const
		{
			return m_Launch.f_GetMemoryStatistics();
		}
		
		CProcessStatistics CVirtualProcessLaunch_Default::f_GetOverallExecutionStatistics() const
		{
			return m_Launch.f_GetOverallExecutionStatistics();
		}
		
		CProcessStatistics CVirtualProcessLaunch_Default::f_GetOverallMemoryStatistics() const
		{
			return m_Launch.f_GetOverallMemoryStatistics();
		}	

		
	}
}

