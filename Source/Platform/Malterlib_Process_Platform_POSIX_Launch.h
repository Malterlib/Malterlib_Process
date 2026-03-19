// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#ifdef DPlatformFamily_macOS
#include <libproc.h>
#include <mach/mach_types.h>
#elif defined(DPlatformFamily_Linux)
#include <sys/time.h>
#include <sys/resource.h>
#endif

#include "Malterlib_Process_Platform_POSIX_LaunchLimiter.h"

#include <Mib/Process/ProcessLaunch>

namespace NMib::NProcess::NPlatform
{
	static_assert(sizeof(pid_t) <= sizeof(umint));
	class CProcessLaunchLink
	{
	public:
		DMibListLinkDS_Link(CProcessLaunchLink, m_Link);
	};

	NStr::CStr fg_FindExecutable(NStr::CStr const &_Path, bool _bAllowLocate, NMib::NFile::EFileAttrib _Type, NContainer::TCVector<NStr::CStr> const &_ExtraPaths = {}, NStr::CStr const &_LocalPaths = {});

	class CPOSIXLaunchContext : public NThread::CThread, public CProcessLaunchLink
	{
		using NThread::CThread::f_Start;
	public:
		CPOSIXLaunchContext();
		~CPOSIXLaunchContext();

		bool f_Open(CProcessLaunchParams const &_Params);

		bool f_RedirectStdInWrite(int &_Pipe, NMib::NStr::CStr &_Errors);

		bool f_Start(EProcessLaunchCloseFlag _Flags);
		void f_Close(EProcessLaunchCloseFlag _Flags);

		bool f_IsRunning();
		void f_SendText(NStr::CStrIO const &_Text);
		void f_CloseStdIn();
		void f_SendBinary(NContainer::CIOByteVector const &_Data);
		fp64 f_GetRunningTime();
		umint f_GetID();
#ifdef DPlatformFamily_macOS
		task_t f_MachTask();
#endif
		void f_Cancel();

		virtual NStr::CStr f_GetThreadName() override;
		virtual aint f_Main() override;

		CProcessStatistics f_OverallMemoryStatistics() const;
		CProcessStatistics f_OverallExecutionStatistics() const;
		bool f_OverallStatsAvailable() const;

		NStorage::CIntrusiveRefCount m_RefCount;

		DIfRefCountDebugging(NStorage::CRefCountDebugReference m_DebugSelfRef);
		DIfRefCountDebugging(NStorage::CRefCountDebugReference m_DebugSelfThreadRef);

	private:
		void fp_OnLaunched(NMib::NStr::CStr const &_Error, void *_pProcess, bool _bSuccess);
		void fp_OnOutput(NMib::NProcess::EProcessLaunchOutputType _OutputType, NMib::NStr::CStr const &_Output);
		void fp_OnExit(uint32 _ExitCode);
		bool fp_DoStart(NStr::CStr &_Errors);
		bool fp_LaunchChild
			(
				int &_hStdOutRead
				, int &_hStdOutWrite
				, int &_hStdInRead
				, int &_hStdInWrite
				, int &_hStdErrRead
				, int &_hStdErrWrite
				, NStr::CStr &_Errors
			)
		;

		void fp_Close();
		virtual bool f_DestroyThread() override;
		void fp_DestroyPipe(int &_Handle);
		void fp_RedirectOutput(bool _bWaitForEOF);
		void fp_UpdateOverallStats
			(
				rusage const &_RUsage
#ifdef DPlatformFamily_macOS
				, proc_taskallinfo const *_pTaskInfo
#endif
			)
		;

		NThread::CMutual m_ExitTimeLock;
		fp64 m_ExitTime;
		NTime::CStopwatch m_TimeSinceStart;

		NMib::NProcess::CProcessLaunchParams mp_LastLaunchOptions;
		NAtomic::TCAtomic<pid_t> mp_ProcessID;
		NThread::CMutual mp_NeedTerminationLock;
		NMib::NProcess::EProcessLaunchCloseFlag mp_NeedTermination;
		bool mp_bNeedWait;
		bool mp_bClosed;
		bool mp_bStarted;
		NAtomic::TCAtomic<uint32> mp_bOverallStatsAvailable;

		CSharedLimiter mp_pCPULimiter;

		mutable NThread::CMutual mp_OverallStatsLock;

		NStorage::TCOptional<rusage> mp_Overall_RUsage;
#ifdef DPlatformFamily_macOS
		NStorage::TCOptional<proc_taskallinfo> mp_Overall_TaskInfo;
#endif
		NThread::CMutual mp_PipeLock;

		int mp_hStdinWrite;	// write end of child's stdin pipe
		int mp_hStdoutRead;	// read end of child's stdout pipe
		int mp_hStderrRead;	// read end of child's stderr pipe

		int mp_WakeupPipeRead;
		int mp_WakeupPipeWrite;
	};
}
