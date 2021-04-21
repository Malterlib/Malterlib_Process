// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include "Malterlib_Process_Platform.h"

#include <Mib/Storage/Variant>

namespace NMib::NProcess
{
	enum EProcessLaunchOutputType
	{
		EProcessLaunchOutputType_StdOut
		, EProcessLaunchOutputType_StdErr
		, EProcessLaunchOutputType_GeneralError
		, EProcessLaunchOutputType_TerminateMessage

		, EProcessLaunchOutputType_Max
	};

	enum EProcessLaunchElevation
	{
		EProcessLaunchElevation_None
		, EProcessLaunchElevation_Elevate
		, EProcessLaunchElevation_DeElevate
	};

	enum EProcessLaunchState
	{
		EProcessLaunchState_Launched			// Variant will be a void * representing the launched process
		, EProcessLaunchState_LaunchFailed		// Variant will be a CStr container the error message
		, EProcessLaunchState_Exited			// Variant will be the exit code of the process
	};

	enum EProcessLaunchType
	{
			EProcessLaunchType_Executable 		// Launch a program.
		,	EProcessLaunchType_Document			// Launch a document (File on a disk or network share etc...)
		,	EProcessLaunchType_URL				// Launch an URL with the OS default application.
	};

	enum EProcessLimit
	{
		// Posix limits
		EProcessLimit_CoreDumpSize				// RLIMIT_CPU			The largest size (in bytes) core file that may be created.
		, EProcessLimit_CpuTime					// RLIMIT_CPU			The maximum amount of cpu time (in seconds) to be used by each process.
		, EProcessLimit_DataSegment				// RLIMIT_DATA			The maximum size (in bytes) of the data segment for a process; this defines how far a program may extend its break with the sbrk(2) system call.
		, EProcessLimit_FileSize				// RLIMIT_FSIZE			The largest size (in bytes) file that may be created.
		, EProcessLimit_LockedMemory			// RLIMIT_MEMLOCK		The maximum size (in bytes) which a process may lock into memory using the mlock(2) function.
		, EProcessLimit_OpenedFiles				// RLIMIT_NOFILE		The maximum number of open files for this process.
		, EProcessLimit_Threads					// RLIMIT_NPROC			The maximum number of simultaneous processes for this user id.
		, EProcessLimit_ResidentMemory			// RLIMIT_RSS			The maximum size (in bytes) to which a process's resident set size may grow.
		, EProcessLimit_StackSize				// RLIMIT_STACK			The maximum size (in bytes) of the stack segment for a process

		// Linux specific limits
		, EProcessLimit_VirtualAddressSpace		// RLIMIT_AS			The maximum size of the process's virtual memory (address space) in bytes.
		, EProcessLimit_MessageQueueSize		// RLIMIT_MSGQUEUE		Specifies the limit on the number of bytes that can be allocated for POSIX message queues for the real user ID of the calling process.
		, EProcessLimit_NiceValue				// RLIMIT_NICE			Specifies a ceiling to which the process's nice value can be raised using setpriority(2) or nice(2).
		, EProcessLimit_RealtimePriority		// RLIMIT_RTPRIO		Specifies a ceiling on the real-time priority that may be set for this process using sched_setscheduler(2) and sched_setparam(2).
		, EProcessLimit_RealtimeTime			// RLIMIT_RTTIME		Specifies a limit (in microseconds) on the amount of CPU time that a process scheduled under a real-time scheduling policy may consume without making a blocking system call.
		, EProcessLimit_SignalsPending			// RLIMIT_SIGPENDING	Specifies the limit on the number of signals that may be queued for the real user ID of the calling process.
	};

	struct CProcessLimit
	{
		uint64 m_MaxValue = (~uint64(0)) - 1;
		uint64 m_Value = (~uint64(0)) - 1;

		void f_SetMaxUnlimited()
		{
			m_MaxValue = ~uint64(0);
		}
		bool f_IsMaxUnlimited() const
		{
			return m_MaxValue == ~uint64(0);
		}
		void f_SetMaxUnchaned()
		{
			m_MaxValue = (~uint64(0)) - 1;
		}
		bool f_IsMaxUnchaned() const
		{
			return m_MaxValue == (~uint64(0)) - 1;
		}

		void f_SetUnlimited()
		{
			m_Value = ~uint64(0);
		}
		bool f_IsUnlimited() const
		{
			return m_Value == ~uint64(0);
		}
		void f_SetUnchaned()
		{
			m_Value = (~uint64(0)) - 1;
		}
		bool f_IsUnchaned() const
		{
			return m_Value == (~uint64(0)) - 1;
		}

		template <typename tf_CStream>
		void f_Feed(tf_CStream &_Stream) const
		{
			_Stream << m_MaxValue;
			_Stream << m_Value;
		}

		template <typename tf_CStream>
		void f_Consume(tf_CStream &_Stream)
		{
			_Stream >> m_MaxValue;
			_Stream >> m_Value;
		}
	};

	typedef NStorage::TCStreamableVariant
		<
			EProcessLaunchState
			, NStorage::TCMember<void *, EProcessLaunchState_Launched>
			, NStorage::TCMember<NMib::NStr::CStr, EProcessLaunchState_LaunchFailed>
			, NStorage::TCMember<uint32, EProcessLaunchState_Exited>
		> 
		CProcessLaunchStateChangeVariant
	;

	struct CProcessLaunchParams
	{
	public:

	public:
		CProcessLaunchParams();
		CProcessLaunchParams(NStr::CStr const &_WorkingDirectory);
		CProcessLaunchParams(CProcessLaunchParams const &_From);
		CProcessLaunchParams(CProcessLaunchParams &&_From);

		CProcessLaunchParams &operator =(CProcessLaunchParams const &_From);
		CProcessLaunchParams &operator =(CProcessLaunchParams &&_From);

		NMib::NStr::CStr m_Operation;			// The operation to do when opering a document
		NMib::NStr::CStr m_Target;				// Can be executable or document depending on m_bLaunchDocument
		NMib::NStr::CStr m_Parameters;
		NMib::NStr::CStr m_WorkingDirectory;
		NMib::NStr::CStr m_ProcessGroup;		// Only if the group is not already created
		NMib::NStr::CStr m_IconPath;			// Path to icon file used by prompt on OSX elevation
		NMib::NStr::CStr m_Prompt;				// Prompt text to use on OSX elevation

		NContainer::TCMap<NMib::NStr::CStr, NMib::NStr::CStr> m_SandboxRoots; // On unix use empty key for setting chroot, on windows use one remapping per drive

		CSystemEnvironment m_Environment;

		NContainer::TCMap<EProcessLimit, CProcessLimit> m_Limits;

		EProcessLaunchType m_LaunchType;

		EExecutionPriority m_LaunchPriority;

		uint32 m_bMergeEnvironment:1;
		uint32 m_bEnableStdRedirection:1;
		uint32 m_bThreaded:1;
		uint32 m_bSeparateStdErr:1;
		uint32 m_bSandboxed:1;
		uint32 m_bCopyRootToSandbox:1;
		uint32 m_bShowLaunched:1;
		uint32 m_bAllowLaunchedInForground:1;
		uint32 m_bAllowExecutableLocate:1;
		uint32 m_bDisplayBusyCursor:1;
		uint32 m_bStdOutPID:1;
		uint32 m_bMakeEffectiveUserReal:1;
		uint32 m_bMakeEffectiveGroupReal:1;
		uint32 m_bCreateNewProcessGroup:1;
		uint32 m_bForceFork:1;

		fp32 m_CPUUsage;		// Percentage of available processing power to use.

		EProcessLaunchElevation m_Elevation;
		NStr::CStr m_RunAsUser;		// Only supported on unix and when running as root
		NStr::CStrSecure m_RunAsUserPassword; // Only used for launches on Windows
		NStr::CStr m_RunAsGroup;	// Only supported on unix and when running as root

		NFunction::TCFunction<void (CProcessLaunchStateChangeVariant const &_State, fp64 _TimeSinceStart)> m_fOnStateChange;

		NFunction::TCFunction<void (EProcessLaunchOutputType _OutputType, NMib::NStr::CStr const &_Output)> m_fOnOutput;
		NFunction::TCFunction<void (NFunction::TCFunction<void ()> const &_Functor)> m_fDispatcher;

		static CProcessLaunchParams fs_LaunchDocument
			(
				NMib::NStr::CStr const &_Operation
				, NMib::NStr::CStr const &_File
				, NFunction::TCFunction<void (CProcessLaunchStateChangeVariant const &_State, fp64 _TimeSinceStart)> const &_fOnStateChange
				, NFunction::TCFunction<void (NFunction::TCFunction<void ()> const &_Functor)> const &_fDispatcher
				= NFunction::TCFunction<void (NFunction::TCFunction<void ()> const &_Functor)>()
			)
		;

		static CProcessLaunchParams fs_LaunchURL
			(
				NMib::NStr::CStr const &_Operation
				, NMib::NStr::CStr const &_URL
				, NFunction::TCFunction<void (CProcessLaunchStateChangeVariant const &_State, fp64 _TimeSinceStart)> const &_fOnStateChange
				, NFunction::TCFunction<void (NFunction::TCFunction<void ()> const &_Functor)> const &_fDispatcher
				= NFunction::TCFunction<void (NFunction::TCFunction<void ()> const &_Functor)>()
			)
		;

		static CProcessLaunchParams fs_LaunchExecutable
			(
				NMib::NStr::CStr const &_Executable
				, NMib::NStr::CStr const &_Parameters
				, NMib::NStr::CStr const &_WorkingDirectory
				, NFunction::TCFunction<void (CProcessLaunchStateChangeVariant const &_State, fp64 _TimeSinceStart)> const &_fOnStateChange
				, NFunction::TCFunction<void (NFunction::TCFunction<void ()> const &_Functor)> const &_fDispatcher
				= NFunction::TCFunction<void (NFunction::TCFunction<void ()> const &_Functor)>()
			)
		;
		static CProcessLaunchParams fs_LaunchExecutable
			(
				NMib::NStr::CStr const &_Executable
				, NContainer::TCVector<NMib::NStr::CStr> const &_Parameters
				, NMib::NStr::CStr const &_WorkingDirectory
				, NFunction::TCFunction<void (CProcessLaunchStateChangeVariant const &_State, fp64 _TimeSinceStart)> const &_fOnStateChange
				, NFunction::TCFunction<void (NFunction::TCFunction<void ()> const &_Functor)> const &_fDispatcher
				= NFunction::TCFunction<void (NFunction::TCFunction<void ()> const &_Functor)>()
			)
		;

		static NStr::CStr fs_GetParamsUnix(NContainer::TCVector<NStr::CStr> const &_Params);
		static NStr::CStr fs_GetParamsBash(NContainer::TCVector<NStr::CStr> const &_Params);
		static NStr::CStr fs_GetParamsWindows(NContainer::TCVector<NStr::CStr> const &_Params);
		static NStr::CStr fs_GetParams(NContainer::TCVector<NStr::CStr> const &_Params);
		static NContainer::TCVector<NStr::CStr> fs_ParseCommandLineWindows(NStr::CStr const &_CommandLine, NStr::CStr &o_Executable);

		enum : uint32
		{
			EProtocolVersion = 0x107
		};

		template <typename tf_CStream>
		void f_Feed(tf_CStream &_Stream) const
		{
			_Stream << EProtocolVersion;
			_Stream << m_Operation;
			_Stream << m_Target;
			_Stream << m_Parameters;
			_Stream << m_WorkingDirectory;
			_Stream << m_ProcessGroup;
			_Stream << m_IconPath;
			_Stream << m_Prompt;

			_Stream << m_SandboxRoots;
			_Stream << m_Environment;
			_Stream << m_Limits;

			_Stream << m_LaunchType;
			_Stream << m_LaunchPriority;

			uint8 Temp = m_bMergeEnvironment;
			_Stream << Temp;
			Temp = m_bEnableStdRedirection;
			_Stream << Temp;
			Temp = m_bThreaded;
			_Stream << Temp;
			Temp = m_bSeparateStdErr;
			_Stream << Temp;
			Temp = m_bSandboxed;
			_Stream << Temp;
			Temp = m_bCopyRootToSandbox;
			_Stream << Temp;
			Temp = m_bShowLaunched;
			_Stream << Temp;
			Temp = m_bAllowLaunchedInForground;
			_Stream << Temp;
			Temp = m_bAllowExecutableLocate;
			_Stream << Temp;
			Temp = m_bDisplayBusyCursor;
			_Stream << Temp;
			Temp = m_bStdOutPID;
			_Stream << Temp;
			Temp = m_bMakeEffectiveUserReal;
			_Stream << Temp;
			Temp = m_bMakeEffectiveGroupReal;
			_Stream << Temp;
			Temp = m_bCreateNewProcessGroup;
			_Stream << Temp;
			Temp = m_bForceFork;
			_Stream << Temp;

			_Stream << m_CPUUsage;
			_Stream << m_Elevation;

			_Stream << m_RunAsUser;
			_Stream << m_RunAsUserPassword;
			_Stream << m_RunAsGroup;
		}

		template <typename tf_CStream>
		void f_Consume(tf_CStream &_Stream)
		{
			uint32 Version;
			_Stream >> Version;
			if (Version > EProtocolVersion || Version < 0x101)
				DMibError("Invalid process launch params protocol version");

			_Stream >> m_Operation;
			_Stream >> m_Target;
			_Stream >> m_Parameters;
			_Stream >> m_WorkingDirectory;
			_Stream >> m_ProcessGroup;
			_Stream >> m_IconPath;
			_Stream >> m_Prompt;

			_Stream >> m_SandboxRoots;
			_Stream >> m_Environment;
			if (Version >= 0x105)
				_Stream >> m_Limits;

			_Stream >> m_LaunchType;
			_Stream >> m_LaunchPriority;

			uint8 Temp;
			_Stream >> Temp;
			m_bMergeEnvironment = Temp;
			_Stream >> Temp;
			m_bEnableStdRedirection = Temp;
			_Stream >> Temp;
			m_bThreaded = Temp;
			_Stream >> Temp;
			m_bSeparateStdErr = Temp;
			_Stream >> Temp;
			m_bSandboxed = Temp;
			_Stream >> Temp;
			m_bCopyRootToSandbox = Temp;
			_Stream >> Temp;
			m_bShowLaunched = Temp;
			_Stream >> Temp;
			m_bAllowLaunchedInForground = Temp;
			_Stream >> Temp;
			m_bAllowExecutableLocate = Temp;
			_Stream >> Temp;
			m_bDisplayBusyCursor = Temp;
			_Stream >> Temp;
			m_bStdOutPID = Temp;

			if (Version >= 0x102)
			{
				_Stream >> Temp;
				m_bMakeEffectiveUserReal = Temp;
				_Stream >> Temp;
				m_bMakeEffectiveGroupReal = Temp;
			}
			if (Version >= 0x104)
			{
				_Stream >> Temp;
				m_bCreateNewProcessGroup = Temp;
			}
			if (Version >= 0x107)
			{
				_Stream >> Temp;
				m_bForceFork = Temp;
			}

			_Stream >> m_CPUUsage;
			_Stream >> m_Elevation;

			if (Version >= 0x103)
			{
				_Stream >> m_RunAsUser;
				if (Version >= 0x106)
					_Stream >> m_RunAsUserPassword;
				_Stream >> m_RunAsGroup;
			}
		}


	};

	class CProcessLaunch
	{
		CProcessLaunch(CProcessLaunch const &) = delete;
		CProcessLaunch &operator = (CProcessLaunch const &) = delete;

		void *m_pProcessLaunch;
		EProcessLaunchCloseFlag m_DestructFlags;
		void fp_CheckOpen() const;
	public:
		CProcessLaunch(CProcessLaunchParams const &_Params, EProcessLaunchCloseFlag _DestructFlags);
		~CProcessLaunch();
		CProcessLaunch(CProcessLaunch &&_Other);

		EProcessLaunchCloseFlag f_GetCloseFlags() const;

		void f_Close(EProcessLaunchCloseFlag _CloseFlags);
		void f_Start(); // Has to be called if _Params.m_bThreaded is not true
		bool f_IsOpen() const;
		bool f_IsRunning() const;
		void f_SendStdIn(NMib::NStr::CStrSecure const &_Data) const;
		void f_CloseStdIn() const;
		void f_SendStdInBinary(NContainer::CSecureByteVector const &_Data) const;
		fp64 f_GetRunningTime() const;
		mint f_GetProcessID() const;
		void f_StopProcess() const; // Soft termination

		CProcessStatistics f_GetExecutionStatistics() const;
		CProcessStatistics f_GetMemoryStatistics() const;

		CProcessStatistics f_GetOverallExecutionStatistics() const;
		CProcessStatistics f_GetOverallMemoryStatistics() const;

		static EProcessElevation fs_GetElevation();

		static NStr::CStr fs_GetBashPath();

		static void fs_RegisterURLHandler(NMib::NStr::CStr const &_Protocol, NMib::NStr::CStr const& _ExePath, NMib::NStr::CStr const &_Params);
		static void fs_DeRegisterURLHandler(NMib::NStr::CStr const &_Protocol);

		// If a platform requires a user identifiable name for a startup entry _Name will be used.
		// If _Name is empty the registered name of the application is used.
		static void fs_RegisterAtStartup(NMib::NStr::CStr const& _ExePath, NMib::NStr::CStr const &_Params, NMib::NStr::CStr const& _Name = NMib::NStr::CStr());
		static void fs_DeRegisterAtStartup(NMib::NStr::CStr const& _ExePath, NMib::NStr::CStr const &_Params, NMib::NStr::CStr const& _Name = NMib::NStr::CStr());

		static void fs_CancelAllLaunches();

		static bool fs_LaunchBlock
			(
				NStr::CStr const &_Executable
				, NStr::CStr const &_Params
				, NStr::CStr &_StdOut
				, NStr::CStr &_StdErr
				, uint32 &_ExitCode
				, CProcessLaunchParams const &_LaunchParams = CProcessLaunchParams()
			)
		;
		static bool fs_LaunchBlock
			(
				NStr::CStr const &_Executable
				, NContainer::TCVector<NStr::CStr> const &_Params
				, NStr::CStr &_StdOut
				, NStr::CStr &_StdErr
				, uint32 &_ExitCode
				, CProcessLaunchParams const &_LaunchParams = CProcessLaunchParams()
			)
		;

		static bool fs_LaunchBlock
			(
				NStr::CStr const &_Executable
				, NStr::CStr const &_Params
				, NFunction::TCFunction<void (NStr::CStr const &_Output)> const &_fOnStdOut
				, NFunction::TCFunction<void (NStr::CStr const &_Output)> const &_fOnStrErr
				, uint32 &_ExitCode
				, CProcessLaunchParams const &_LaunchParams = CProcessLaunchParams()
			)
		;
		static bool fs_LaunchBlock
			(
				NStr::CStr const &_Executable
				, NContainer::TCVector<NStr::CStr> const &_Params
				, NFunction::TCFunction<void (NStr::CStr const &_Output)> const &_fOnStdOut
				, NFunction::TCFunction<void (NStr::CStr const &_Output)> const &_fOnStrErr
				, uint32 &_ExitCode
				, CProcessLaunchParams const &_LaunchParams = CProcessLaunchParams()
			)
		;
		static NStr::CStr fs_LaunchTool
			(
				NStr::CStr const &_Executable
				, NContainer::TCVector<NStr::CStr> const &_Params
				, CProcessLaunchParams const &_LaunchParams = CProcessLaunchParams()
			)
		;

		static mint fs_KillProcesses(NFunction::TCFunction<bool (CProcessInfo const &_ProcessInfo)> const &_fProcessFilter, EProcessInfoFlag _InfoFlags, fp64 _Timeout = 30.0);
		static mint fs_KillProcessesInDirectory(NStr::CStr const &_NamePattern, NStr::CStr const &_ArgsPattern = {}, NStr::CStr const &_Directory = {}, fp64 _Timeout = 30.0);
	};
}

#ifndef DMibPNoShortCuts
	using namespace NMib::NProcess;
#endif
