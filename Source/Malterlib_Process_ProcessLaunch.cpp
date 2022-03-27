// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>

#include "Malterlib_Process_ProcessLaunch.h"

namespace NMib::NProcess
{
	void CProcessLaunch::fp_CheckOpen() const
	{
		if (!m_pProcessLaunch)
			DMibError("Process launch not open");
	}
	CProcessLaunch::CProcessLaunch(CProcessLaunch &&_Other)
		: m_pProcessLaunch(_Other.m_pProcessLaunch)
		, m_DestructFlags(_Other.m_DestructFlags)
	{
		_Other.m_pProcessLaunch = nullptr;
	}

	CProcessLaunch::CProcessLaunch(CProcessLaunchParams const &_Params, EProcessLaunchCloseFlag _DestructFlags)
		: m_pProcessLaunch(nullptr)
		, m_DestructFlags(_DestructFlags & ~EProcessLaunchCloseFlag_CloseInProgress)
	{
		m_pProcessLaunch = NPlatform::fg_ProcessLaunch_Open(_Params);
		if (_Params.m_bThreaded)
			 NPlatform::fg_ProcessLaunch_Start(m_pProcessLaunch, _DestructFlags);
	}

	CProcessLaunch::~CProcessLaunch()
	{
		if (m_pProcessLaunch)
			f_Close(m_DestructFlags);
	}

	void CProcessLaunch::f_Start()
	{
		NPlatform::fg_ProcessLaunch_Start(m_pProcessLaunch, m_DestructFlags);
	}

	EProcessElevation CProcessLaunch::fs_GetElevation()
	{
		return NPlatform::fg_Process_GetElevation();
	}

	NStr::CStr CProcessLaunch::fs_GetBashPath()
	{
#ifdef DPlatformFamily_Windows
		NStr::CStr GitBash = "C:/Program Files/Git/usr/bin/bash.exe";
		if (NFile::CFile::fs_FileExists(GitBash))
			return GitBash;
#endif
		return "bash";
	}

	void CProcessLaunch::fs_RegisterURLHandler(NMib::NStr::CStr const &_Protocol, NMib::NStr::CStr const& _ExePath, NMib::NStr::CStr const &_Params)
	{
		return NPlatform::fg_Process_RegisterURLHandler(_Protocol, _ExePath, _Params);
	}

	void CProcessLaunch::fs_DeRegisterURLHandler(NMib::NStr::CStr const &_Protocol)
	{
		return NPlatform::fg_Process_DeRegisterURLHandler(_Protocol);
	}

	void CProcessLaunch::fs_RegisterAtStartup(NMib::NStr::CStr const& _ExePath, NMib::NStr::CStr const &_Params, NMib::NStr::CStr const& _Name)
	{
		return NPlatform::fg_Process_RegisterAtStartup(_ExePath, _Params, _Name);
	}

	void CProcessLaunch::fs_DeRegisterAtStartup(NMib::NStr::CStr const& _ExePath, NMib::NStr::CStr const &_Params, NMib::NStr::CStr const& _Name)
	{
		return NPlatform::fg_Process_DeRegisterAtStartup(_ExePath, _Params, _Name);
	}

	void CProcessLaunch::fs_CancelAllLaunches()
	{
		return NPlatform::fg_ProcessLaunch_CancelAll();
	}

	bool CProcessLaunch::fs_LaunchBlock
		(
			NStr::CStr const &_Executable
			, NContainer::TCVector<NStr::CStr> const &_Params
			, NStr::CStr &_StdOut
			, NStr::CStr &_StdErr
			, uint32 &_ExitCode
			, CProcessLaunchParams const &_LaunchParams
		)
	{
		return fs_LaunchBlock(_Executable, CProcessLaunchParams::fs_GetParams(_Params), _StdOut, _StdErr, _ExitCode, _LaunchParams);
	}

	bool CProcessLaunch::fs_LaunchBlock
		(
			NStr::CStr const &_Executable
			, NStr::CStr const &_Params
			, NStr::CStr &_StdOut
			, NStr::CStr &_StdErr
			, uint32 &_ExitCode
			, CProcessLaunchParams const &_LaunchParams
		)
	{
		return fs_LaunchBlock
			(
				_Executable
				, _Params
				, [&](NStr::CStr const &_Output)
				{
					_StdOut += _Output;
				}
				, [&](NStr::CStr const &_Output)
				{
					_StdErr += _Output;
				}
				, _ExitCode
				, _LaunchParams
			)
		;
	}

	bool CProcessLaunch::fs_LaunchBlock
		(
			NStr::CStr const &_Executable
			, NContainer::TCVector<NStr::CStr> const &_Params
			, NFunction::TCFunction<void (NStr::CStr const &_Output)> const &_fOnStdOut
			, NFunction::TCFunction<void (NStr::CStr const &_Output)> const &_fOnStrErr
			, uint32 &_ExitCode
			, CProcessLaunchParams const &_LaunchParams
		)
	{
		return fs_LaunchBlock(_Executable, CProcessLaunchParams::fs_GetParams(_Params), _fOnStdOut, _fOnStrErr, _ExitCode, _LaunchParams);
	}


	bool CProcessLaunch::fs_LaunchBlock
		(
			NStr::CStr const &_Executable
			, NStr::CStr const &_Params
			, NFunction::TCFunction<void (NStr::CStr const &_Output)> const &_fOnStdOut
			, NFunction::TCFunction<void (NStr::CStr const &_Output)> const &_fOnStrErr
			, uint32 &_ExitCode
			, CProcessLaunchParams const &_LaunchParams
		)
	{
		NMib::NProcess::CProcessLaunchParams Params = _LaunchParams;

		Params.m_Target = _Executable;
		Params.m_Parameters = _Params;
		Params.m_bSeparateStdErr = true;
		Params.m_bAllowExecutableLocate = true;
		bool bRet = false;

		//Params.
		Params.m_fOnStateChange
			= [&](NMib::NProcess::CProcessLaunchStateChangeVariant const &_State, fp64 _TimeSinceStart)
			{
				switch (_State.f_GetTypeID())
				{
					case NMib::NProcess::EProcessLaunchState_Exited:
					{
						_ExitCode = _State.f_Get<NMib::NProcess::EProcessLaunchState_Exited>();
					}
					break;
					case NMib::NProcess::EProcessLaunchState_LaunchFailed:
					{
						_fOnStrErr(_State.f_Get<NMib::NProcess::EProcessLaunchState_LaunchFailed>());
					}
					break;
					case NMib::NProcess::EProcessLaunchState_Launched:
					{
						bRet = true;
					}
					break;
				}
			}
		;

		Params.m_fOnOutput
			= [&](NMib::NProcess::EProcessLaunchOutputType _OutputType, NMib::NStr::CStr const &_Output)
			{
				if (_OutputType == NMib::NProcess::EProcessLaunchOutputType_StdOut)
					_fOnStdOut(_Output);
				else
					_fOnStrErr(_Output);
			}
		;

		{
			NMib::NProcess::CProcessLaunch ProcessLaunch(Params, NMib::NProcess::EProcessLaunchCloseFlag_BlockOnExit);
		}

		return bRet;
	}

	NStr::CStr CProcessLaunch::fs_LaunchTool
		(
			NStr::CStr const &_Executable
			, NContainer::TCVector<NStr::CStr> const &_Params
			, CProcessLaunchParams const &_LaunchParams
		)
	{
		NStr::CStr StdOut;
		NStr::CStr StdErr;
		uint32 ExitCode = 1;
		if (!CProcessLaunch::fs_LaunchBlock(_Executable, _Params, StdOut, StdErr, ExitCode, _LaunchParams))
			DMibError(fg_Format("{} failed to launch: {}", _Executable, StdErr));
		if (ExitCode)
			DMibError(fg_Format("{} exited with {}: {}", _Executable, ExitCode, StdErr.f_TrimRight()));
		return StdOut;
	}

	void CProcessLaunch::f_Close(EProcessLaunchCloseFlag _CloseFlags)
	{
		if (m_pProcessLaunch && !(m_DestructFlags & EProcessLaunchCloseFlag_CloseInProgress))
		{
			m_DestructFlags = _CloseFlags;
			m_DestructFlags |= EProcessLaunchCloseFlag_CloseInProgress;

			auto Cleanup = g_OnScopeExit / [this]
				{
					m_DestructFlags = m_DestructFlags & ~EProcessLaunchCloseFlag_CloseInProgress;
				}
			;

			NPlatform::fg_ProcessLaunch_Close(m_pProcessLaunch, _CloseFlags);
			m_pProcessLaunch = nullptr;

			Cleanup.f_Clear();
		}
	}

	EProcessLaunchCloseFlag CProcessLaunch::f_GetCloseFlags() const
	{
		if (m_pProcessLaunch)
			DMibError("Process launch still open");

		return m_DestructFlags;
	}

	bool CProcessLaunch::f_IsOpen() const
	{
		return m_pProcessLaunch != nullptr;
	}

	bool CProcessLaunch::f_IsRunning() const
	{
		fp_CheckOpen();
		return NPlatform::fg_ProcessLaunch_IsRunning(m_pProcessLaunch);
	}

	void CProcessLaunch::f_SendStdIn(NMib::NStr::CStrSecure const &_Data) const
	{
		fp_CheckOpen();
		NPlatform::fg_ProcessLaunch_SendStdIn(m_pProcessLaunch, _Data);
	}

	void CProcessLaunch::f_CloseStdIn() const
	{
		fp_CheckOpen();
		NPlatform::fg_ProcessLaunch_CloseStdIn(m_pProcessLaunch);
	}

	void CProcessLaunch::f_SendStdInBinary(NContainer::CSecureByteVector const &_Data) const
	{
		fp_CheckOpen();
		NPlatform::fg_ProcessLaunch_SendStdInBinary(m_pProcessLaunch, _Data);
	}

	fp64 CProcessLaunch::f_GetRunningTime() const
	{
		fp_CheckOpen();
		return NPlatform::fg_ProcessLaunch_GetRunningTime(m_pProcessLaunch);
	}

	mint CProcessLaunch::f_GetProcessID() const
	{
		return NPlatform::fg_ProcessLaunch_GetID(m_pProcessLaunch);
	}

	void CProcessLaunch::f_StopProcess() const
	{
		NPlatform::fg_ProcessLaunch_Stop(m_pProcessLaunch);
	}

	void CProcessLaunch::f_StopProcessGroup() const
	{
		NPlatform::fg_ProcessLaunch_StopGroup(m_pProcessLaunch);
	}

	CProcessStatistics CProcessLaunch::f_GetExecutionStatistics() const
	{
		return NPlatform::fg_ProcessLaunch_GetExecutionStatistics(m_pProcessLaunch);
	}

	CProcessStatistics CProcessLaunch::f_GetMemoryStatistics() const
	{
		return NPlatform::fg_ProcessLaunch_GetMemoryStatistics(m_pProcessLaunch);
	}

	CProcessStatistics CProcessLaunch::f_GetOverallExecutionStatistics() const
	{
		return NPlatform::fg_ProcessLaunch_GetOverallExecutionStatistics(m_pProcessLaunch);
	}

	CProcessStatistics CProcessLaunch::f_GetOverallMemoryStatistics() const
	{
		return NPlatform::fg_ProcessLaunch_GetOverallMemoryStatistics(m_pProcessLaunch);
	}

	NContainer::TCVector<NStr::CStr> CProcessLaunchParams::fs_ParseCommandLineWindows(NStr::CStr const &_CommandLine, NStr::CStr &o_Executable)
	{
		ch8 const *pParse = _CommandLine.f_GetStr();

		auto fIsWhitespace = [](ch8 _Char)
			{
				return _Char == ' ' || _Char == '\t';
			}
		;

		while (*pParse && fIsWhitespace(*pParse))
			++pParse;

		if (*pParse == '\"')
		{
			++pParse;
			auto pStart = pParse;
			while (*pParse && *pParse != '\"')
				++pParse;

			o_Executable = NStr::CStr(pStart, pParse - pStart);

			if (*pParse == '\"')
				++pParse;
		}
		else
		{
			auto pStart = pParse;
			while (*pParse && !fIsWhitespace(*pParse))
				++pParse;

			o_Executable = NStr::CStr(pStart, pParse - pStart);
		}

		NContainer::TCVector<NStr::CStr> Params;

		bool bInQuotes = false;

		while (*pParse)
		{
			while (*pParse && fIsWhitespace(*pParse))
				++pParse;

			if (!*pParse)
				break;

			NStr::CStr Param;
			while (true)
			{
				bool bShouldCopy = true;
				mint nSlashes = 0;

				while (*pParse == '\\')
				{
					++pParse;
					++nSlashes;
				}

				if (*pParse == '"')
				{
					if (nSlashes % 2 == 0)
					{
						if (bInQuotes && pParse[1] == '"')
							pParse++;
						else
						{
							bShouldCopy = false;
							bInQuotes = !bInQuotes;
						}
					}

					nSlashes /= 2;
				}

				while (nSlashes--)
					Param.f_AddChar('\\');

				if (!*pParse || (!bInQuotes && fIsWhitespace(*pParse)))
					break;

				if (bShouldCopy)
					Param.f_AddChar(*pParse);

				++pParse;
			}
			Params.f_Insert(Param);
		}

		return Params;
	}

	NStr::CStr CProcessLaunchParams::fs_GetParamsBash(NContainer::TCVector<NStr::CStr> const &_Params)
	{
		NStr::CStr Params;
		for (auto &Param : _Params)
		{
			if (fg_StrEscapeBashQuotesNeeded(Param))
				NStr::fg_AddStrSep(Params, fg_StrEscapeBashDoubleQuotes(Param), ' ');
			else
				NStr::fg_AddStrSep(Params, Param, ' ');
		}

		return Params;
	}

	NStr::CStr CProcessLaunchParams::fs_GetParamsWindows(NContainer::TCVector<NStr::CStr> const &_Params)
	{
		// Rules:
		// 2N     backslashes   + " ==> N backslashes and begin/end quote
		// 2N + 1 backslashes   + " ==> N backslashes + literal "
		// N      backslashes       ==> N backslashes

		NStr::CStr Params;
		for (auto iParam = _Params.f_GetIterator(); iParam; ++iParam)
		{
			auto &Param = *iParam;

			if (!Params.f_IsEmpty())
				Params.f_AddChar(' ');

			if (Param.f_IsEmpty())
				Params += "\"\"";
			else if (Param.f_FindChars(" \"") >= 0)
			{
				Params += "\"";

				ch8 const *pParse = Param;
				mint nBackslashes = 0;
				while (*pParse)
				{
					ch8 Char = *pParse;
					if (Char == '\"')
					{
						for (mint i = 0; i < nBackslashes; ++i)
							Params.f_AddChar('\\');
						Params += "\\\"";
						nBackslashes = 0;
					}
					else if (Char == '\\')
						++nBackslashes;
					else
					{
						nBackslashes = 0;
						Params.f_AddChar(Char);
					}
					++pParse;
				}

				for (mint i = 0; i < nBackslashes; ++i)
					Params.f_AddChar('\\');

				Params += "\"";
			}
			else
				Params += Param;
		}

		return Params;
	}

	NStr::CStr CProcessLaunchParams::fs_GetParamsUnix(NContainer::TCVector<NStr::CStr> const &_Params)
	{
		NStr::CStr Params;
		for (auto iParam = _Params.f_GetIterator(); iParam; ++iParam)
			NStr::fg_AddStrSepEscaped(Params, *iParam, ' ');

		return Params;
	}

	NStr::CStr CProcessLaunchParams::fs_GetParams(NContainer::TCVector<NStr::CStr> const &_Params)
	{
#ifdef DPlatformFamily_Windows
		return fs_GetParamsWindows(_Params);
#else
		return fs_GetParamsUnix(_Params);
#endif
	}

	CProcessLaunchParams CProcessLaunchParams::fs_LaunchDocument
		(
			NMib::NStr::CStr const &_Operation
			, NMib::NStr::CStr const &_File
			, NFunction::TCFunction<void (CProcessLaunchStateChangeVariant const &_State, fp64 _TimeSinceStart)> const &_fOnStateChange
			, NFunction::TCFunction<void (NFunction::TCFunction<void ()> const &_Functor)> const &_Dispatcher
		)
	{
		CProcessLaunchParams Ret;
		Ret.m_LaunchType = EProcessLaunchType_Document;
		Ret.m_Operation = _Operation;
		Ret.m_Target = _File;
		Ret.m_fOnStateChange = _fOnStateChange;
		Ret.m_fDispatcher = _Dispatcher;
		return Ret;
	}

	CProcessLaunchParams CProcessLaunchParams::fs_LaunchURL
		(
			NMib::NStr::CStr const &_Operation
			, NMib::NStr::CStr const &_URL
			, NFunction::TCFunction<void (CProcessLaunchStateChangeVariant const &_State, fp64 _TimeSinceStart)> const &_fOnStateChange
			, NFunction::TCFunction<void (NFunction::TCFunction<void ()> const &_Functor)> const &_Dispatcher
		)
	{
		CProcessLaunchParams Ret;
		Ret.m_LaunchType = EProcessLaunchType_URL;
		Ret.m_Operation = _Operation;
		Ret.m_Target = _URL;
		Ret.m_fOnStateChange = _fOnStateChange;
		Ret.m_fDispatcher = _Dispatcher;
		return Ret;
	}

	CProcessLaunchParams CProcessLaunchParams::fs_LaunchExecutableRawParams
		(
			NMib::NStr::CStr const &_Executable
			, NMib::NStr::CStr const &_Parameters
			, NMib::NStr::CStr const &_WorkingDirectory
			, NFunction::TCFunction<void (CProcessLaunchStateChangeVariant const &_State, fp64 _TimeSinceStart)> const &_fOnStateChange
			, NFunction::TCFunction<void (NFunction::TCFunction<void ()> const &_Functor)> const &_Dispatcher
		)
	{
		CProcessLaunchParams Ret;
		Ret.m_LaunchType = EProcessLaunchType_Executable;
		Ret.m_Target = _Executable;
		Ret.m_Parameters = _Parameters;
		Ret.m_WorkingDirectory = _WorkingDirectory;
		Ret.m_fOnStateChange = _fOnStateChange;
		Ret.m_fDispatcher = _Dispatcher;
		return Ret;
	}

	CProcessLaunchParams CProcessLaunchParams::fs_LaunchExecutable
		(
			NMib::NStr::CStr const &_Executable
			, NContainer::TCVector<NMib::NStr::CStr> const &_Parameters
			, NMib::NStr::CStr const &_WorkingDirectory
			, NFunction::TCFunction<void (CProcessLaunchStateChangeVariant const &_State, fp64 _TimeSinceStart)> const &_fOnStateChange
			, NFunction::TCFunction<void (NFunction::TCFunction<void ()> const &_Functor)> const &_Dispatcher
		)
	{
		return fs_LaunchExecutableRawParams(_Executable, fs_GetParams(_Parameters), _WorkingDirectory, _fOnStateChange, _Dispatcher);
	}

	CProcessLaunchParams &CProcessLaunchParams::operator =(CProcessLaunchParams const &_From)
	{
		m_Operation = _From.m_Operation;
		m_Target = _From.m_Target;
		m_Parameters = _From.m_Parameters;
		m_IconPath = _From.m_IconPath;
		m_Prompt = _From.m_Prompt;
		m_WorkingDirectory = _From.m_WorkingDirectory;
		m_Environment = _From.m_Environment;
		m_Limits = _From.m_Limits;
		m_LaunchType = _From.m_LaunchType;
		m_LaunchPriority = _From.m_LaunchPriority;
		m_bMergeEnvironment = _From.m_bMergeEnvironment;
		m_SandboxRoots = _From.m_SandboxRoots;
		m_ProcessGroup = _From.m_ProcessGroup;
		m_bSeparateStdErr = _From.m_bSeparateStdErr;
		m_bEnableStdRedirection = _From.m_bEnableStdRedirection;
		m_bThreaded = _From.m_bThreaded;
		m_bSandboxed = _From.m_bSandboxed;
		m_bCopyRootToSandbox = _From.m_bCopyRootToSandbox;
		m_bShowLaunched = _From.m_bShowLaunched;
		m_bAllowLaunchedInForground = _From.m_bAllowLaunchedInForground;
		m_bAllowExecutableLocate = _From.m_bAllowExecutableLocate;
		m_bDisplayBusyCursor = _From.m_bDisplayBusyCursor;
		m_bStdOutPID = _From.m_bStdOutPID;
		m_bMakeEffectiveUserReal = _From.m_bMakeEffectiveUserReal;
		m_bMakeEffectiveGroupReal = _From.m_bMakeEffectiveGroupReal;
		m_bCreateNewProcessGroup = _From.m_bCreateNewProcessGroup;
		m_bForceFork = _From.m_bForceFork;
		m_CPUUsage = _From.m_CPUUsage;
		m_Elevation = _From.m_Elevation;
		m_RunAsUser = _From.m_RunAsUser;
		m_RunAsUserPassword = _From.m_RunAsUserPassword;
		m_RunAsGroup = _From.m_RunAsGroup;
		m_fOnStateChange = _From.m_fOnStateChange;
		m_fOnOutput = _From.m_fOnOutput;
		m_fDispatcher = _From.m_fDispatcher;
		return *this;
	}

	CProcessLaunchParams::CProcessLaunchParams(NStr::CStr const &_WorkingDirectory)
		: CProcessLaunchParams()
	{
		m_WorkingDirectory = _WorkingDirectory;
	}

	CProcessLaunchParams::CProcessLaunchParams()
		: m_bMergeEnvironment(1)
		, m_bEnableStdRedirection(1)
		, m_LaunchType(EProcessLaunchType_Executable)
		, m_bThreaded(1)
		, m_bSeparateStdErr(1)
		, m_bSandboxed(0)
		, m_bCopyRootToSandbox(1)
		, m_LaunchPriority(EExecutionPriority_Default)
		, m_bShowLaunched(1)
		, m_Elevation(EProcessLaunchElevation_None)
		, m_Environment()
		, m_bAllowLaunchedInForground(1)
		, m_bAllowExecutableLocate(0)
		, m_bDisplayBusyCursor(false)
		, m_bStdOutPID(false)
		, m_CPUUsage(0)
		, m_bMakeEffectiveUserReal(false)
		, m_bMakeEffectiveGroupReal(false)
		, m_bCreateNewProcessGroup(false)
		, m_bForceFork(false)
	{
	}


	CProcessLaunchParams::CProcessLaunchParams(CProcessLaunchParams const &_From)
		: m_Operation(_From.m_Operation)
		, m_Target(_From.m_Target)
		, m_Parameters(_From.m_Parameters)
		, m_IconPath(_From.m_IconPath)
		, m_Prompt(_From.m_Prompt)
		, m_WorkingDirectory(_From.m_WorkingDirectory)
		, m_Environment(_From.m_Environment)
		, m_Limits(_From.m_Limits)
		, m_LaunchType(_From.m_LaunchType)
		, m_LaunchPriority(_From.m_LaunchPriority)
		, m_bMergeEnvironment(_From.m_bMergeEnvironment)
		, m_SandboxRoots(_From.m_SandboxRoots)
		, m_ProcessGroup(_From.m_ProcessGroup)
		, m_bEnableStdRedirection(_From.m_bEnableStdRedirection)
		, m_bThreaded(_From.m_bThreaded)
		, m_bSeparateStdErr(_From.m_bSeparateStdErr)
		, m_bSandboxed(_From.m_bSandboxed)
		, m_bCopyRootToSandbox(_From.m_bCopyRootToSandbox)
		, m_bShowLaunched(_From.m_bShowLaunched)
		, m_bAllowLaunchedInForground(_From.m_bAllowLaunchedInForground)
		, m_bAllowExecutableLocate(_From.m_bAllowExecutableLocate)
		, m_bDisplayBusyCursor(_From.m_bDisplayBusyCursor)
		, m_bStdOutPID(_From.m_bStdOutPID)
		, m_CPUUsage(_From.m_CPUUsage)
		, m_Elevation(_From.m_Elevation)
		, m_fOnStateChange(_From.m_fOnStateChange)
		, m_fOnOutput(_From.m_fOnOutput)
		, m_fDispatcher(_From.m_fDispatcher)
		, m_bMakeEffectiveUserReal(_From.m_bMakeEffectiveUserReal)
		, m_bMakeEffectiveGroupReal(_From.m_bMakeEffectiveGroupReal)
		, m_bCreateNewProcessGroup(_From.m_bCreateNewProcessGroup)
		, m_bForceFork(_From.m_bForceFork)
		, m_RunAsUser(_From.m_RunAsUser)
		, m_RunAsUserPassword(_From.m_RunAsUserPassword)
		, m_RunAsGroup(_From.m_RunAsGroup)
	{
	}

	CProcessLaunchParams::CProcessLaunchParams(CProcessLaunchParams &&_From)
		: m_Operation(fg_Move(_From.m_Operation))
		, m_Target(fg_Move(_From.m_Target))
		, m_Parameters(fg_Move(_From.m_Parameters))
		, m_IconPath(fg_Move(_From.m_IconPath))
		, m_Prompt(fg_Move(_From.m_Prompt))
		, m_WorkingDirectory(fg_Move(_From.m_WorkingDirectory))
		, m_Environment(fg_Move(_From.m_Environment))
		, m_Limits(fg_Move(_From.m_Limits))
		, m_LaunchType(_From.m_LaunchType)
		, m_LaunchPriority(_From.m_LaunchPriority)
		, m_bMergeEnvironment(_From.m_bMergeEnvironment)
		, m_SandboxRoots(_From.m_SandboxRoots)
		, m_ProcessGroup(_From.m_ProcessGroup)
		, m_bSeparateStdErr(_From.m_bSeparateStdErr)
		, m_bEnableStdRedirection(_From.m_bEnableStdRedirection)
		, m_bThreaded(_From.m_bThreaded)
		, m_bSandboxed(_From.m_bSandboxed)
		, m_bCopyRootToSandbox(_From.m_bCopyRootToSandbox)
		, m_bShowLaunched(_From.m_bShowLaunched)
		, m_bAllowLaunchedInForground(_From.m_bAllowLaunchedInForground)
		, m_bAllowExecutableLocate(_From.m_bAllowExecutableLocate)
		, m_bDisplayBusyCursor(_From.m_bDisplayBusyCursor)
		, m_bStdOutPID(_From.m_bStdOutPID)
		, m_CPUUsage(_From.m_CPUUsage)
		, m_Elevation(_From.m_Elevation)
		, m_fOnStateChange(fg_Move(_From.m_fOnStateChange))
		, m_fOnOutput(fg_Move(_From.m_fOnOutput))
		, m_fDispatcher(fg_Move(_From.m_fDispatcher))
		, m_bMakeEffectiveUserReal(_From.m_bMakeEffectiveUserReal)
		, m_bMakeEffectiveGroupReal(_From.m_bMakeEffectiveGroupReal)
		, m_bCreateNewProcessGroup(_From.m_bCreateNewProcessGroup)
		, m_bForceFork(_From.m_bForceFork)
		, m_RunAsUser(fg_Move(_From.m_RunAsUser))
		, m_RunAsUserPassword(fg_Move(_From.m_RunAsUserPassword))
		, m_RunAsGroup(fg_Move(_From.m_RunAsGroup))
	{
	}

	CProcessLaunchParams &CProcessLaunchParams::operator =(CProcessLaunchParams &&_From)
	{
		m_Operation = fg_Move(_From.m_Operation);
		m_Target = fg_Move(_From.m_Target);
		m_Parameters = fg_Move(_From.m_Parameters);
		m_IconPath = fg_Move(_From.m_IconPath);
		m_Prompt = fg_Move(_From.m_Prompt);
		m_WorkingDirectory = fg_Move(_From.m_WorkingDirectory);
		m_Environment = fg_Move(_From.m_Environment);
		m_Limits = fg_Move(_From.m_Limits);
		m_LaunchType = _From.m_LaunchType;
		m_LaunchPriority = _From.m_LaunchPriority;
		m_bMergeEnvironment = _From.m_bMergeEnvironment;
		m_SandboxRoots = _From.m_SandboxRoots;
		m_ProcessGroup = _From.m_ProcessGroup;
		m_bSeparateStdErr = _From.m_bSeparateStdErr;
		m_bEnableStdRedirection = _From.m_bEnableStdRedirection;
		m_bThreaded = _From.m_bThreaded;
		m_bSandboxed = _From.m_bSandboxed;
		m_bCopyRootToSandbox = _From.m_bCopyRootToSandbox;
		m_bShowLaunched = _From.m_bShowLaunched;
		m_bAllowLaunchedInForground = _From.m_bAllowLaunchedInForground;
		m_bAllowExecutableLocate = _From.m_bAllowExecutableLocate;
		m_bDisplayBusyCursor = _From.m_bDisplayBusyCursor;
		m_bStdOutPID = _From.m_bStdOutPID;
		m_CPUUsage = _From.m_CPUUsage;
		m_Elevation = _From.m_Elevation;
		m_fOnStateChange = fg_Move(_From.m_fOnStateChange);
		m_fOnOutput = fg_Move(_From.m_fOnOutput);
		m_fDispatcher = fg_Move(_From.m_fDispatcher);
		m_bMakeEffectiveUserReal = _From.m_bMakeEffectiveUserReal;
		m_bMakeEffectiveGroupReal = _From.m_bMakeEffectiveGroupReal;
		m_bCreateNewProcessGroup = _From.m_bCreateNewProcessGroup;
		m_bForceFork = _From.m_bForceFork;
		m_RunAsUser = fg_Move(_From.m_RunAsUser);
		m_RunAsUserPassword = fg_Move(_From.m_RunAsUserPassword);
		m_RunAsGroup = fg_Move(_From.m_RunAsGroup);
		return *this;
	}
}
