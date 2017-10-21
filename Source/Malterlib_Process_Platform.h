// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

namespace NMib
{
	namespace NProcess
	{
		struct CProcessLaunchParams;
		
		enum EProcessLaunchCloseFlag
		{
			EProcessLaunchCloseFlag_None = 0
			, EProcessLaunchCloseFlag_TerminateProcess = DMibBit(0)
			, EProcessLaunchCloseFlag_LingerUntilDone = DMibBit(1)
			, EProcessLaunchCloseFlag_BlockOnExit = DMibBit(2)
			, EProcessLaunchCloseFlag_StopProcess = DMibBit(3)
		};

		enum EProcessElevation
		{
			EProcessElevation_None = 0
			, EProcessElevation_IsNotElevated	// Implies that you can elevate
			, EProcessElevation_IsElevated		// Implies that you can de-elavate
			, EProcessElevation_IsRoot
		};

		
		enum EProcessStatUnit
		{
			EProcessStatUnit_GeneralNumber // Use m_CustomUnit
			, EProcessStatUnit_Bytes
			, EProcessStatUnit_Cycles
			, EProcessStatUnit_Seconds
			, EProcessStatUnit_Fraction
		};
		
		struct CProcessStat
		{
			EProcessStatUnit m_Unit;
			NMib::NStr::CStr m_CustomUnit;
			fp64 m_Value;
			fp64 m_IdealScale;
			CProcessStat(EProcessStatUnit _Unit, fp64 _Value, fp64 _IdealScale = 1.0, NMib::NStr::CStr const &_CustomUnit = NMib::NStr::CStr())
				: m_Unit(_Unit)
				, m_CustomUnit(_CustomUnit)
				, m_Value(_Value)
				, m_IdealScale(_IdealScale)
			{
			}
		};
		
		struct CProcessStatistics
		{
			NMib::NContainer::TCMap<NMib::NStr::CStr, CProcessStat> m_Statistics;
		};
		
		struct CVersionInfo
		{
			uint16 m_Major;
			uint16 m_Minor;
			uint16 m_Revision;
			uint16 m_MinorRevision;
			NStr::CStr m_Branch;
			NTime::CTime m_BuildTime;

			uint64 f_GetFullVersion()
			{
				return (uint64(m_Major) << 48) | (uint64(m_Minor) << 32) | (uint64(m_Revision) << 16) | uint64(m_MinorRevision);
			}
		};

		struct CStdInReaderParams;
		
		enum EProcessInfoFlag
		{
			EProcessInfoFlag_None = 0
			, EProcessInfoFlag_ParentProcessID = DMibBit(0)
			, EProcessInfoFlag_StartTime = DMibBit(1)
			, EProcessInfoFlag_FileName = DMibBit(2)
			, EProcessInfoFlag_FullPath = DMibBit(3)
			, EProcessInfoFlag_Args = DMibBit(4)
		};

		struct CProcessInfo
		{
			mint m_ProcessID;								// Always returned
			mint m_ParentProcessID;							// Returned for EProcessInfoFlag_ParentProcessID 
			uint64 m_StartTime;								// Returned for EProcessInfoFlag_StartTime
			NMib::NStr::CStr m_FileName;					// Returned for EProcessInfoFlag_FileName
			NMib::NStr::CStr m_FullPath;					// Returned for EProcessInfoFlag_FullPath
			NContainer::TCVector<NMib::NStr::CStr> m_Args;	// Returned for EProcessInfoFlag_Args
		};		
		
		namespace NPlatform
		{
			NMib::NStr::CStr fg_Process_GetComputerDomain();
			uint64 fg_Process_GetPhysicalMemory();
			
			void fg_Process_Pause(mint _ProcessID);
			void fg_Process_Resume(mint _ProcessID);
			void fg_Process_Stop(mint _ProcessID);
			void fg_Process_Terminate(mint _ProcessID);
	 
			mint fg_Process_GetCurrentUID();
			bint fg_Process_GetProcessIsParentProcess(mint _ProcessID);
			bool fg_Process_IsRunning(mint _ProcessID);
			
			NMib::NStr::CStr fg_Process_GetUserName();
			NMib::NStr::CStr fg_Process_GetComputerName();
			NMib::NStr::CStr fg_Process_GetComputerAddress();
			NMib::NStr::CStr fg_Process_GetHostName();

			void fg_Process_SetPriority(uint16 _Priority);

			NContainer::TCVector<NProcess::CProcessInfo> fg_Process_Enum(NProcess::EProcessInfoFlag _ToGet = NProcess::EProcessInfoFlag_None, NContainer::TCVector<NProcess::CProcessInfo> * _pOldEnum = nullptr);
			
			void fg_Process_GetMemoryCurrentStatistics(void *_pProcess, NProcess::CProcessStatistics &_Stats);
			void fg_Process_GetMemoryOverallStatistics(void *_pProcess, NProcess::CProcessStatistics &_Stats);

			void fg_Process_GetExecutionCurrentStatistics(void *_pProcess, NProcess::CProcessStatistics &_Stats);
			void fg_Process_GetExecutionOverallStatistics(void *_pProcess, NProcess::CProcessStatistics &_Stats);

			void *fg_Process_StdInReader_Open(NMib::NProcess::CStdInReaderParams &&_Params);
			void fg_Process_StdInReader_Close(void *_pStdInReader);

			void *fg_ProcessLaunch_Open(NMib::NProcess::CProcessLaunchParams const &_Params);
			void fg_ProcessLaunch_Start(void *_pLaunch, NMib::NProcess::EProcessLaunchCloseFlag _DestructFlags);
			void fg_ProcessLaunch_Close(void *_pLaunch, NMib::NProcess::EProcessLaunchCloseFlag _Flags);
			bint fg_ProcessLaunch_IsRunning(void *_pLaunch);
			void fg_ProcessLaunch_SendStdIn(void *_pLaunch, NMib::NStr::CStrSecure const &_Data);
			void fg_ProcessLaunch_SendStdInBinary(void *_pLaunch, NContainer::TCVector<uint8, NMem::CAllocator_HeapSecure> const &_Data);

			fp64 fg_ProcessLaunch_GetRunningTime(void *_pLaunch);
			mint fg_ProcessLaunch_GetID(void *_pLaunch);
			void fg_ProcessLaunch_Stop(void *_pLaunch);
			
			void fg_Process_WaitForTermination();

			NProcess::CProcessStatistics fg_ProcessLaunch_GetExecutionStatistics(void *_pLaunch);
			NProcess::CProcessStatistics fg_ProcessLaunch_GetMemoryStatistics(void *_pLaunch);
			NProcess::CProcessStatistics fg_ProcessLaunch_GetOverallExecutionStatistics(void *_pLaunch);
			NProcess::CProcessStatistics fg_ProcessLaunch_GetOverallMemoryStatistics(void *_pLaunch);
			
			void fg_ProcessLaunch_CancelAll();

			NMib::NProcess::EProcessElevation fg_Process_GetElevation();

			void fg_Process_RegisterURLHandler(NMib::NStr::CStr const &_Protocol, NMib::NStr::CStr const& _ExePath, NMib::NStr::CStr const &_Params);
			void fg_Process_DeRegisterURLHandler(NMib::NStr::CStr const &_Protocol);

			// If a platform requires a user identifiable name for a startup entry _Name will be used.
			// If _Name is empty the registered name of the application is used.
			void fg_Process_RegisterAtStartup(NMib::NStr::CStr const& _ExePath, NMib::NStr::CStr const &_Params, NMib::NStr::CStr const& _Name = NMib::NStr::CStr());
			void fg_Process_DeRegisterAtStartup(NMib::NStr::CStr const& _ExePath, NMib::NStr::CStr const &_Params, NMib::NStr::CStr const& _Name = NMib::NStr::CStr());

			void fg_Process_GetVersionInfo(NMib::NStr::CStr const &_File, NProcess::CVersionInfo &_VersionInfo);
				
			NMib::NStr::CStr fg_Process_GetOperatingSystemTag(int32 _MajorMax, int32 _MinorMax);
			NMib::NStr::CStr fg_Process_GetOperatingSystemDescription();
		}
	}
}

#ifndef DMibPNoShortCuts
	using namespace NMib::NProcess;	
#endif

