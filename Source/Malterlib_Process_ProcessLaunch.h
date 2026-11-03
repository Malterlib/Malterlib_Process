// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

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
		EProcessLaunchState_Launched       // Variant will be a void * representing the launched process
		, EProcessLaunchState_LaunchFailed // Variant will be a CStr container the error message
		, EProcessLaunchState_Exited       // Variant will be the exit code of the process
	};

	enum EProcessLaunchType
	{
		EProcessLaunchType_Executable // Launch a program.
		, EProcessLaunchType_Document // Launch a document (File on a disk or network share etc...)
		, EProcessLaunchType_URL      // Launch an URL with the OS default application.
	};

	enum EProcessLimit
	{
		// Posix limits
		EProcessLimit_CoreDumpSize     // RLIMIT_CPU     The largest size (in bytes) core file that may be created.
		, EProcessLimit_CpuTime        // RLIMIT_CPU     The maximum amount of cpu time (in seconds) to be used by each process.
		, EProcessLimit_DataSegment    // RLIMIT_DATA    The maximum size (in bytes) of the data segment for a process, which limits how far sbrk(2) may extend its break.
		, EProcessLimit_FileSize       // RLIMIT_FSIZE   The largest size (in bytes) file that may be created.
		, EProcessLimit_LockedMemory   // RLIMIT_MEMLOCK The maximum size (in bytes) which a process may lock into memory using the mlock(2) function.
		, EProcessLimit_OpenedFiles    // RLIMIT_NOFILE  The maximum number of open files for this process.
		, EProcessLimit_Threads        // RLIMIT_NPROC   The maximum number of simultaneous processes for this user id.
		, EProcessLimit_ResidentMemory // RLIMIT_RSS     The maximum size (in bytes) to which a process's resident set size may grow.
		, EProcessLimit_StackSize      // RLIMIT_STACK   The maximum size (in bytes) of the stack segment for a process

		// Linux specific limits
		, EProcessLimit_VirtualAddressSpace // RLIMIT_AS         The maximum size of the process's virtual memory (address space) in bytes.
		, EProcessLimit_MessageQueueSize    // RLIMIT_MSGQUEUE   The number of bytes that can be allocated for POSIX message queues for the real user ID of the calling process.
		, EProcessLimit_NiceValue           // RLIMIT_NICE       Specifies a ceiling to which the process's nice value can be raised using setpriority(2) or nice(2).
		, EProcessLimit_RealtimePriority    // RLIMIT_RTPRIO     A ceiling on the real-time priority this process may set using sched_setscheduler(2) and sched_setparam(2).
		, EProcessLimit_RealtimeTime        // RLIMIT_RTTIME     CPU time (in microseconds) a process under a real-time scheduling policy may use without a blocking system call.
		, EProcessLimit_SignalsPending      // RLIMIT_SIGPENDING Specifies the limit on the number of signals that may be queued for the real user ID of the calling process.
	};

	struct CProcessLimit
	{
		uint64 m_MaxValue = (~uint64(0)) - 1;
		uint64 m_Value = (~uint64(0)) - 1;

		void f_SetMaxUnlimited();
		bool f_IsMaxUnlimited() const;
		void f_SetMaxUnchaned();
		bool f_IsMaxUnchaned() const;
		void f_SetUnlimited();
		bool f_IsUnlimited() const;
		void f_SetUnchaned();
		bool f_IsUnchaned() const;

		template <typename tf_CStream>
		void f_Feed(tf_CStream &_Stream) const;
		template <typename tf_CStream>
		void f_Consume(tf_CStream &_Stream);
	};

	using CProcessLaunchStateChangeVariant = NStorage::TCStreamableVariant
		<
			EProcessLaunchState
			, NStorage::TCMember<void *, EProcessLaunchState_Launched>
			, NStorage::TCMember<NMib::NStr::CStr, EProcessLaunchState_LaunchFailed>
			, NStorage::TCMember<uint32, EProcessLaunchState_Exited>
		>
	;

	struct CProcessLaunchParams
	{
		CProcessLaunchParams();
		CProcessLaunchParams(NStr::CStr const &_WorkingDirectory);
		CProcessLaunchParams(CProcessLaunchParams const &_From);
		CProcessLaunchParams(CProcessLaunchParams &&_From);

		CProcessLaunchParams &operator = (CProcessLaunchParams const &_From);
		CProcessLaunchParams &operator = (CProcessLaunchParams &&_From);

		NMib::NStr::CStr m_Operation;    // The operation to do when opering a document
		NMib::NStr::CStr m_Target;       // Can be executable or document depending on m_bLaunchDocument
		NMib::NStr::CStr m_Parameters;
		NMib::NStr::CStr m_WorkingDirectory;
		NMib::NStr::CStr m_ProcessGroup; // Only if the group is not already created
		NMib::NStr::CStr m_IconPath;     // Path to icon file used by prompt on macOS elevation
		NMib::NStr::CStr m_Prompt;       // Prompt text to use on macOS elevation

		NContainer::TCMap<NMib::NStr::CStr, NMib::NStr::CStr> m_SandboxRoots; // On unix use empty key for setting chroot, on windows use one remapping per drive

		CSystemEnvironment m_Environment;

		NContainer::TCMap<EProcessLimit, CProcessLimit> m_Limits;

		EProcessLaunchType m_LaunchType = EProcessLaunchType_Executable;

		EExecutionPriority m_LaunchPriority = EExecutionPriority_Default;

		uint32 m_bMergeEnvironment:1 = true;
		uint32 m_bEnableStdRedirection:1 = true;
		uint32 m_bThreaded:1 = true;
		uint32 m_bSeparateStdErr:1 = true;
		uint32 m_bSandboxed:1 = false;
		uint32 m_bCopyRootToSandbox:1 = true;
		uint32 m_bShowLaunched:1 = true;
		uint32 m_bAllowLaunchedInForeground:1 = true;
		uint32 m_bAllowExecutableLocate:1 = false;
		uint32 m_bDisplayBusyCursor:1 = false;
		uint32 m_bStdOutPID:1 = false;
		uint32 m_bMakeEffectiveUserReal:1 = false;
		uint32 m_bMakeEffectiveGroupReal:1 = false;
		uint32 m_bCreateNewProcessGroup:1 = false;
		uint32 m_bForceFork:1 = false;
		// Together with m_RunAsUser on macOS this will launch through "launchctl asuser" to provide the correct bootstrap namespace for things like XPC
		uint32 m_bLaunchInUserSession:1 = true;

		fp32 m_CPUUsage = 0.0f; // Percentage of available processing power to use.

		EProcessLaunchElevation m_Elevation = EProcessLaunchElevation_None;
		NStr::CStr m_RunAsUser;               // Only supported on unix and when running as root
		NStr::CStrSecure m_RunAsUserPassword; // Only used for launches on Windows
		NStr::CStr m_RunAsGroup;              // Only supported on unix and when running as root

		NFunction::TCFunction<void (CProcessLaunchStateChangeVariant const &_State, fp64 _TimeSinceStart)> m_fOnStateChange;

		NFunction::TCFunction<void (EProcessLaunchOutputType _OutputType, NMib::NStr::CStr const &_Output)> m_fOnOutput;
		// Stdout and stderr as read, in place of m_fOnOutput, which still gets the other output. Proxies carry output as text
		NFunction::TCFunction<void (EProcessLaunchOutputType _OutputType, NContainer::CIOByteVector const &_Output)> m_fOnOutputBinary;
		NFunction::TCFunction<void (NFunction::TCFunction<void ()> const &_fFunctor)> m_fDispatcher;

		static auto fs_LaunchDocument
			(
				NMib::NStr::CStr const &_Operation
				, NMib::NStr::CStr const &_File
				, NFunction::TCFunction<void (CProcessLaunchStateChangeVariant const &_State, fp64 _TimeSinceStart)> const &_fOnStateChange
				, NFunction::TCFunction<void (NFunction::TCFunction<void ()> const &_fFunctor)> const &_fDispatcher
				= NFunction::TCFunction<void (NFunction::TCFunction<void ()> const &_fFunctor)>()
			)
			-> CProcessLaunchParams
		;

		static auto fs_LaunchURL
			(
				NMib::NStr::CStr const &_Operation
				, NMib::NStr::CStr const &_URL
				, NFunction::TCFunction<void (CProcessLaunchStateChangeVariant const &_State, fp64 _TimeSinceStart)> const &_fOnStateChange
				, NFunction::TCFunction<void (NFunction::TCFunction<void ()> const &_fFunctor)> const &_fDispatcher
				= NFunction::TCFunction<void (NFunction::TCFunction<void ()> const &_fFunctor)>()
			)
			-> CProcessLaunchParams
		;

		static auto fs_LaunchExecutableRawParams
			(
				NMib::NStr::CStr const &_Executable
				, NMib::NStr::CStr const &_Parameters
				, NMib::NStr::CStr const &_WorkingDirectory
				, NFunction::TCFunction<void (CProcessLaunchStateChangeVariant const &_State, fp64 _TimeSinceStart)> const &_fOnStateChange
				, NFunction::TCFunction<void (NFunction::TCFunction<void ()> const &_fFunctor)> const &_fDispatcher
				= NFunction::TCFunction<void (NFunction::TCFunction<void ()> const &_fFunctor)>()
			)
			-> CProcessLaunchParams
		;
		static auto fs_LaunchExecutable
			(
				NMib::NStr::CStr const &_Executable
				, NContainer::TCVector<NMib::NStr::CStr> const &_Parameters
				, NMib::NStr::CStr const &_WorkingDirectory
				, NFunction::TCFunction<void (CProcessLaunchStateChangeVariant const &_State, fp64 _TimeSinceStart)> const &_fOnStateChange
				, NFunction::TCFunction<void (NFunction::TCFunction<void ()> const &_fFunctor)> const &_fDispatcher
				= NFunction::TCFunction<void (NFunction::TCFunction<void ()> const &_fFunctor)>()
			)
			-> CProcessLaunchParams
		;

		static NStr::CStr fs_GetParamsUnix(NContainer::TCVector<NStr::CStr> const &_Params);
		static NStr::CStr fs_GetParamsBash(NContainer::TCVector<NStr::CStr> const &_Params);
		static NStr::CStr fs_GetParamsWindows(NContainer::TCVector<NStr::CStr> const &_Params);
		static NStr::CStr fs_GetParams(NContainer::TCVector<NStr::CStr> const &_Params);
		static NContainer::TCVector<NStr::CStr> fs_ParseCommandLineUnix(NStr::CStr const &_CommandLine, NStr::CStr &o_Executable);
		static NContainer::TCVector<NStr::CStr> fs_ParseCommandLineWindows(NStr::CStr const &_CommandLine, NStr::CStr &o_Executable);

		enum EProtocolVersion : uint32
		{
			EProtocolVersion_Current = 0x108
		};

		template <typename tf_CStream>
		void f_Feed(tf_CStream &_Stream) const;
		template <typename tf_CStream>
		void f_Consume(tf_CStream &_Stream);
	};

	struct CProcessLaunch
	{
		CProcessLaunch(CProcessLaunchParams const &_Params, EProcessLaunchCloseFlag _DestructFlags);
		~CProcessLaunch();
		CProcessLaunch(CProcessLaunch &&_Other);

		EProcessLaunchCloseFlag f_GetCloseFlags() const;

		void f_Close(EProcessLaunchCloseFlag _CloseFlags);
		void f_Start(); // Has to be called if _Params.m_bThreaded is not true
		bool f_IsOpen() const;
		bool f_IsRunning() const;
		void f_SendStdIn(NMib::NStr::CStrIO const &_Data) const;
		void f_CloseStdIn() const;
		void f_SendStdInBinary(NContainer::CIOByteVector const &_Data) const;
		fp64 f_GetRunningTime() const;
		umint f_GetProcessID() const;
		void f_StopProcess() const; // Soft termination
		void f_StopProcessGroup() const;
		void f_TerminateProcessTree() const; // Kills the process and its descendants and leaves the launch open

		CProcessStatistics f_GetExecutionStatistics() const;
		CProcessStatistics f_GetMemoryStatistics() const;

		CProcessStatistics f_GetOverallExecutionStatistics() const;
		CProcessStatistics f_GetOverallMemoryStatistics() const;

		static EProcessElevation fs_GetElevation();

		static NStr::CStr fs_GetBashPath();

		static void fs_RegisterURLHandler(NMib::NStr::CStr const &_Protocol, NMib::NStr::CStr const &_ExePath, NMib::NStr::CStr const &_Params);
		static void fs_DeRegisterURLHandler(NMib::NStr::CStr const &_Protocol);

		// If a platform requires a user identifiable name for a startup entry _Name will be used.
		// If _Name is empty the registered name of the application is used.
		static void fs_RegisterAtStartup(NMib::NStr::CStr const &_ExePath, NMib::NStr::CStr const &_Params, NMib::NStr::CStr const &_Name = NMib::NStr::CStr());
		static void fs_DeRegisterAtStartup(NMib::NStr::CStr const &_ExePath, NMib::NStr::CStr const &_Params, NMib::NStr::CStr const &_Name = NMib::NStr::CStr());

		static void fs_CancelAllLaunches();

		static auto fs_LaunchBlock
			(
				NStr::CStr const &_Executable
				, NStr::CStr const &_Params
				, NStr::CStr &_StdOut
				, NStr::CStr &_StdErr
				, uint32 &_ExitCode
				, CProcessLaunchParams const &_LaunchParams = CProcessLaunchParams()
			)
			-> bool
		;
		static auto fs_LaunchBlock
			(
				NStr::CStr const &_Executable
				, NContainer::TCVector<NStr::CStr> const &_Params
				, NStr::CStr &_StdOut
				, NStr::CStr &_StdErr
				, uint32 &_ExitCode
				, CProcessLaunchParams const &_LaunchParams = CProcessLaunchParams()
			)
			-> bool
		;

		static auto fs_LaunchBlock
			(
				NStr::CStr const &_Executable
				, NStr::CStr const &_Params
				, NFunction::TCFunction<void (NStr::CStr const &_Output)> const &_fOnStdOut
				, NFunction::TCFunction<void (NStr::CStr const &_Output)> const &_fOnStrErr
				, uint32 &_ExitCode
				, CProcessLaunchParams const &_LaunchParams = CProcessLaunchParams()
			)
			-> bool
		;
		static auto fs_LaunchBlock
			(
				NStr::CStr const &_Executable
				, NContainer::TCVector<NStr::CStr> const &_Params
				, NFunction::TCFunction<void (NStr::CStr const &_Output)> const &_fOnStdOut
				, NFunction::TCFunction<void (NStr::CStr const &_Output)> const &_fOnStrErr
				, uint32 &_ExitCode
				, CProcessLaunchParams const &_LaunchParams = CProcessLaunchParams()
			)
			-> bool
		;
		static NStr::CStr fs_LaunchTool(NStr::CStr const &_Executable, NContainer::TCVector<NStr::CStr> const &_Params, CProcessLaunchParams const &_LaunchParams = CProcessLaunchParams());

		static umint fs_KillProcesses(NFunction::TCFunction<bool (CProcessInfo const &_ProcessInfo)> const &_fProcessFilter, EProcessInfoFlag _InfoFlags, fp64 _Timeout = 30.0);
		static umint fs_KillProcessesInDirectory(NStr::CStr const &_NamePattern, NStr::CStr const &_ArgsPattern = {}, NStr::CStr const &_Directory = {}, fp64 _Timeout = 30.0);

	private:
		CProcessLaunch(CProcessLaunch const &_Other) = delete;
		CProcessLaunch &operator = (CProcessLaunch const &_Other) = delete;

		void *mp_pProcessLaunch;
		EProcessLaunchCloseFlag mp_DestructFlags;
		void fp_CheckOpen() const;
	};
}

#ifndef DMibPNoShortCuts
using namespace NMib::NProcess;
#endif

#include "Malterlib_Process_ProcessLaunch.hpp"
