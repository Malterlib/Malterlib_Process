// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Core/Core>

#include "Malterlib_Process_ProcessLaunch.h"

namespace NMib::NProcess
{
	void CProcessLimit::f_SetMaxUnlimited()
	{
		m_MaxValue = ~uint64(0);
	}

	bool CProcessLimit::f_IsMaxUnlimited() const
	{
		return m_MaxValue == ~uint64(0);
	}

	void CProcessLimit::f_SetMaxUnchaned()
	{
		m_MaxValue = (~uint64(0)) - 1;
	}

	bool CProcessLimit::f_IsMaxUnchaned() const
	{
		return m_MaxValue == (~uint64(0)) - 1;
	}

	void CProcessLimit::f_SetUnlimited()
	{
		m_Value = ~uint64(0);
	}

	bool CProcessLimit::f_IsUnlimited() const
	{
		return m_Value == ~uint64(0);
	}

	void CProcessLimit::f_SetUnchaned()
	{
		m_Value = (~uint64(0)) - 1;
	}

	bool CProcessLimit::f_IsUnchaned() const
	{
		return m_Value == (~uint64(0)) - 1;
	}

	void CProcessLaunch::fp_CheckOpen() const
	{
		if (!mp_pProcessLaunch)
			DMibError("Process launch not open");
	}

	CProcessLaunch::CProcessLaunch(CProcessLaunch &&_Other)
		: mp_pProcessLaunch(_Other.mp_pProcessLaunch)
		, mp_DestructFlags(_Other.mp_DestructFlags)
	{
		_Other.mp_pProcessLaunch = nullptr;
	}

	CProcessLaunch::CProcessLaunch(CProcessLaunchParams const &_Params, EProcessLaunchCloseFlag _DestructFlags)
		: mp_pProcessLaunch(nullptr)
		, mp_DestructFlags(_DestructFlags & ~EProcessLaunchCloseFlag_CloseInProgress)
	{
		mp_pProcessLaunch = NPlatform::fg_ProcessLaunch_Open(_Params);
		if (_Params.m_bThreaded)
			NPlatform::fg_ProcessLaunch_Start(mp_pProcessLaunch, _DestructFlags);
	}

	CProcessLaunch::~CProcessLaunch()
	{
		if (mp_pProcessLaunch)
			f_Close(mp_DestructFlags);
	}

	void CProcessLaunch::f_Start()
	{
		NPlatform::fg_ProcessLaunch_Start(mp_pProcessLaunch, mp_DestructFlags);
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

	void CProcessLaunch::fs_RegisterURLHandler(NMib::NStr::CStr const &_Protocol, NMib::NStr::CStr const &_ExePath, NMib::NStr::CStr const &_Params)
	{
		return NPlatform::fg_Process_RegisterURLHandler(_Protocol, _ExePath, _Params);
	}

	void CProcessLaunch::fs_DeRegisterURLHandler(NMib::NStr::CStr const &_Protocol)
	{
		return NPlatform::fg_Process_DeRegisterURLHandler(_Protocol);
	}

	void CProcessLaunch::fs_RegisterAtStartup(NMib::NStr::CStr const &_ExePath, NMib::NStr::CStr const &_Params, NMib::NStr::CStr const &_Name)
	{
		return NPlatform::fg_Process_RegisterAtStartup(_ExePath, _Params, _Name);
	}

	void CProcessLaunch::fs_DeRegisterAtStartup(NMib::NStr::CStr const &_ExePath, NMib::NStr::CStr const &_Params, NMib::NStr::CStr const &_Name)
	{
		return NPlatform::fg_Process_DeRegisterAtStartup(_ExePath, _Params, _Name);
	}

	void CProcessLaunch::fs_CancelAllLaunches()
	{
		return NPlatform::fg_ProcessLaunch_CancelAll();
	}

	auto CProcessLaunch::fs_LaunchBlock
		(
			NStr::CStr const &_Executable
			, NContainer::TCVector<NStr::CStr> const &_Params
			, NStr::CStr &_StdOut
			, NStr::CStr &_StdErr
			, uint32 &_ExitCode
			, CProcessLaunchParams const &_LaunchParams
		)
		-> bool
	{
		return fs_LaunchBlock(_Executable, CProcessLaunchParams::fs_GetParams(_Params), _StdOut, _StdErr, _ExitCode, _LaunchParams);
	}

	auto CProcessLaunch::fs_LaunchBlock
		(
			NStr::CStr const &_Executable
			, NStr::CStr const &_Params
			, NStr::CStr &_StdOut
			, NStr::CStr &_StdErr
			, uint32 &_ExitCode
			, CProcessLaunchParams const &_LaunchParams
		)
		-> bool
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

	auto CProcessLaunch::fs_LaunchBlock
		(
			NStr::CStr const &_Executable
			, NContainer::TCVector<NStr::CStr> const &_Params
			, NFunction::TCFunction<void (NStr::CStr const &_Output)> const &_fOnStdOut
			, NFunction::TCFunction<void (NStr::CStr const &_Output)> const &_fOnStrErr
			, uint32 &_ExitCode
			, CProcessLaunchParams const &_LaunchParams
		)
		-> bool
	{
		return fs_LaunchBlock(_Executable, CProcessLaunchParams::fs_GetParams(_Params), _fOnStdOut, _fOnStrErr, _ExitCode, _LaunchParams);
	}

	auto CProcessLaunch::fs_LaunchBlock
		(
			NStr::CStr const &_Executable
			, NStr::CStr const &_Params
			, NFunction::TCFunction<void (NStr::CStr const &_Output)> const &_fOnStdOut
			, NFunction::TCFunction<void (NStr::CStr const &_Output)> const &_fOnStrErr
			, uint32 &_ExitCode
			, CProcessLaunchParams const &_LaunchParams
		)
		-> bool
	{
		NMib::NProcess::CProcessLaunchParams Params = _LaunchParams;

		Params.m_Target = _Executable;
		Params.m_Parameters = _Params;
		Params.m_bSeparateStdErr = true;
		Params.m_bAllowExecutableLocate = true;
		bool bRet = false;

		//Params.
		Params.m_fOnStateChange = [&](NMib::NProcess::CProcessLaunchStateChangeVariant const &_State, fp64 _TimeSinceStart)
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

		Params.m_fOnOutput = [&](NMib::NProcess::EProcessLaunchOutputType _OutputType, NMib::NStr::CStr const &_Output)
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

	NStr::CStr CProcessLaunch::fs_LaunchTool(NStr::CStr const &_Executable, NContainer::TCVector<NStr::CStr> const &_Params, CProcessLaunchParams const &_LaunchParams)
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
		if (mp_pProcessLaunch && !(mp_DestructFlags & EProcessLaunchCloseFlag_CloseInProgress))
		{
			mp_DestructFlags = _CloseFlags;
			mp_DestructFlags |= EProcessLaunchCloseFlag_CloseInProgress;

			auto Cleanup = g_OnScopeExit / [this]
				{
					mp_DestructFlags = mp_DestructFlags & ~EProcessLaunchCloseFlag_CloseInProgress;
				}
			;

			NPlatform::fg_ProcessLaunch_Close(mp_pProcessLaunch, _CloseFlags);
			mp_pProcessLaunch = nullptr;

			Cleanup.f_Clear();
		}
	}

	EProcessLaunchCloseFlag CProcessLaunch::f_GetCloseFlags() const
	{
		if (mp_pProcessLaunch)
			DMibError("Process launch still open");

		return mp_DestructFlags;
	}

	bool CProcessLaunch::f_IsOpen() const
	{
		return mp_pProcessLaunch != nullptr;
	}

	bool CProcessLaunch::f_IsRunning() const
	{
		fp_CheckOpen();
		return NPlatform::fg_ProcessLaunch_IsRunning(mp_pProcessLaunch);
	}

	void CProcessLaunch::f_SendStdIn(NMib::NStr::CStrIO const &_Data) const
	{
		fp_CheckOpen();
		NPlatform::fg_ProcessLaunch_SendStdIn(mp_pProcessLaunch, _Data);
	}

	void CProcessLaunch::f_CloseStdIn() const
	{
		fp_CheckOpen();
		NPlatform::fg_ProcessLaunch_CloseStdIn(mp_pProcessLaunch);
	}

	void CProcessLaunch::f_SendStdInBinary(NContainer::CIOByteVector const &_Data) const
	{
		fp_CheckOpen();
		NPlatform::fg_ProcessLaunch_SendStdInBinary(mp_pProcessLaunch, _Data);
	}

	fp64 CProcessLaunch::f_GetRunningTime() const
	{
		fp_CheckOpen();
		return NPlatform::fg_ProcessLaunch_GetRunningTime(mp_pProcessLaunch);
	}

	umint CProcessLaunch::f_GetProcessID() const
	{
		return NPlatform::fg_ProcessLaunch_GetID(mp_pProcessLaunch);
	}

	void CProcessLaunch::f_StopProcess() const
	{
		NPlatform::fg_ProcessLaunch_Stop(mp_pProcessLaunch);
	}

	void CProcessLaunch::f_StopProcessGroup() const
	{
		NPlatform::fg_ProcessLaunch_StopGroup(mp_pProcessLaunch);
	}

	void CProcessLaunch::f_TerminateProcessTree() const
	{
		NPlatform::fg_ProcessLaunch_TerminateTree(mp_pProcessLaunch);
	}

	CProcessStatistics CProcessLaunch::f_GetExecutionStatistics() const
	{
		return NPlatform::fg_ProcessLaunch_GetExecutionStatistics(mp_pProcessLaunch);
	}

	CProcessStatistics CProcessLaunch::f_GetMemoryStatistics() const
	{
		return NPlatform::fg_ProcessLaunch_GetMemoryStatistics(mp_pProcessLaunch);
	}

	CProcessStatistics CProcessLaunch::f_GetOverallExecutionStatistics() const
	{
		return NPlatform::fg_ProcessLaunch_GetOverallExecutionStatistics(mp_pProcessLaunch);
	}

	CProcessStatistics CProcessLaunch::f_GetOverallMemoryStatistics() const
	{
		return NPlatform::fg_ProcessLaunch_GetOverallMemoryStatistics(mp_pProcessLaunch);
	}

	// Parses fs_GetParamsUnix's direct-execution format, not shell syntax. Double quotes group text;
	// backslashes escape the next byte only inside quotes. Unclosed quotes extend to the end of input.
	NContainer::TCVector<NStr::CStr> CProcessLaunchParams::fs_ParseCommandLineUnix(NStr::CStr const &_CommandLine, NStr::CStr &o_Executable)
	{
		// A CStr copy shares storage without allocating; retain it only when the output aliases the input.
		NStr::CStr InputOwner;
		if (&_CommandLine == &o_Executable)
			InputOwner = _CommandLine;
		ch8 const *pParse = _CommandLine.f_GetStr();
		o_Executable.f_Clear();

		auto fIsWhitespace = [](ch8 _Char)
			{
				return _Char == ' ' || _Char == '\t' || _Char == '\r' || _Char == '\n';
			}
		;
		auto fScanToken = [&](ch8 const * &o_pParse, ch8 *_pOutput) -> umint
			{
				bool bInQuotes = false;
				umint Length = 0;
				while (*o_pParse && (bInQuotes || !fIsWhitespace(*o_pParse)))
				{
					ch8 Character = *o_pParse++;
					if (Character == '"')
					{
						bInQuotes = !bInQuotes;

						continue;
					}
					if (bInQuotes && Character == '\\')
					{
						if (!*o_pParse)
							break;
						Character = *o_pParse++;
					}
					if (_pOutput)
						_pOutput[Length] = Character;
					++Length;
				}

				return Length;
			}
		;
		auto fReadToken = [&](NStr::CStr &o_Token)
			{
				auto *pStart = pParse;
				umint Length = fScanToken(pParse, nullptr);
				if (Length)
				{
					auto *pOutput = o_Token.f_GetStr(Length);
					fScanToken(pStart, pOutput);
					pOutput[Length] = 0;
				}
			}
		;

		while (fIsWhitespace(*pParse))
			++pParse;
		if (*pParse)
			fReadToken(o_Executable);

		NContainer::TCVector<NStr::CStr> Params;
		for (;;)
		{
			while (fIsWhitespace(*pParse))
				++pParse;
			if (!*pParse)
				break;

			fReadToken(Params.f_Insert());
		}

		return Params;
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
				umint nSlashes = 0;

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
				umint nBackslashes = 0;
				while (*pParse)
				{
					ch8 Char = *pParse;
					if (Char == '\"')
					{
						// 2N+1 backslashes + " ==> N backslashes + literal "
						for (umint iBackslash = 0; iBackslash < nBackslashes * 2 + 1; ++iBackslash)
							Params.f_AddChar('\\');
						Params.f_AddChar('"');
						nBackslashes = 0;
					}
					else if (Char == '\\')
						++nBackslashes;
					else
					{
						// N backslashes followed by a non-quote char are literal
						for (umint iBackslash = 0; iBackslash < nBackslashes; ++iBackslash)
							Params.f_AddChar('\\');
						nBackslashes = 0;
						Params.f_AddChar(Char);
					}
					++pParse;
				}

				// Trailing backslashes before the closing quote must be doubled so they
				// round-trip as N literal backslashes (2N backslashes + " ==> N + quote).
				for (umint iBackslash = 0; iBackslash < nBackslashes * 2; ++iBackslash)
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
			NStr::fg_AddStrSepEscaped(Params, *iParam, ' ', "\"\\\t\r\n");

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

	auto CProcessLaunchParams::fs_LaunchDocument
		(
			NMib::NStr::CStr const &_Operation
			, NMib::NStr::CStr const &_File
			, NFunction::TCFunction<void (CProcessLaunchStateChangeVariant const &_State, fp64 _TimeSinceStart)> const &_fOnStateChange
			, NFunction::TCFunction<void (NFunction::TCFunction<void ()> const &_fFunctor)> const &_fDispatcher
		)
		-> CProcessLaunchParams
	{
		CProcessLaunchParams Ret;
		Ret.m_LaunchType = EProcessLaunchType_Document;
		Ret.m_Operation = _Operation;
		Ret.m_Target = _File;
		Ret.m_fOnStateChange = _fOnStateChange;
		Ret.m_fDispatcher = _fDispatcher;
		return Ret;
	}

	auto CProcessLaunchParams::fs_LaunchURL
		(
			NMib::NStr::CStr const &_Operation
			, NMib::NStr::CStr const &_URL
			, NFunction::TCFunction<void (CProcessLaunchStateChangeVariant const &_State, fp64 _TimeSinceStart)> const &_fOnStateChange
			, NFunction::TCFunction<void (NFunction::TCFunction<void ()> const &_fFunctor)> const &_fDispatcher
		)
		-> CProcessLaunchParams
	{
		CProcessLaunchParams Ret;
		Ret.m_LaunchType = EProcessLaunchType_URL;
		Ret.m_Operation = _Operation;
		Ret.m_Target = _URL;
		Ret.m_fOnStateChange = _fOnStateChange;
		Ret.m_fDispatcher = _fDispatcher;
		return Ret;
	}

	auto CProcessLaunchParams::fs_LaunchExecutableRawParams
		(
			NMib::NStr::CStr const &_Executable
			, NMib::NStr::CStr const &_Parameters
			, NMib::NStr::CStr const &_WorkingDirectory
			, NFunction::TCFunction<void (CProcessLaunchStateChangeVariant const &_State, fp64 _TimeSinceStart)> const &_fOnStateChange
			, NFunction::TCFunction<void (NFunction::TCFunction<void ()> const &_fFunctor)> const &_fDispatcher
		)
		-> CProcessLaunchParams
	{
		CProcessLaunchParams Ret;
		Ret.m_LaunchType = EProcessLaunchType_Executable;
		Ret.m_Target = _Executable;
		Ret.m_Parameters = _Parameters;
		Ret.m_WorkingDirectory = _WorkingDirectory;
		Ret.m_fOnStateChange = _fOnStateChange;
		Ret.m_fDispatcher = _fDispatcher;
		return Ret;
	}

	auto CProcessLaunchParams::fs_LaunchExecutable
		(
			NMib::NStr::CStr const &_Executable
			, NContainer::TCVector<NMib::NStr::CStr> const &_Parameters
			, NMib::NStr::CStr const &_WorkingDirectory
			, NFunction::TCFunction<void (CProcessLaunchStateChangeVariant const &_State, fp64 _TimeSinceStart)> const &_fOnStateChange
			, NFunction::TCFunction<void (NFunction::TCFunction<void ()> const &_fFunctor)> const &_fDispatcher
		)
		-> CProcessLaunchParams
	{
		return fs_LaunchExecutableRawParams(_Executable, fs_GetParams(_Parameters), _WorkingDirectory, _fOnStateChange, _fDispatcher);
	}

	CProcessLaunchParams::CProcessLaunchParams(NStr::CStr const &_WorkingDirectory)
		: CProcessLaunchParams()
	{
		m_WorkingDirectory = _WorkingDirectory;
	}

	CProcessLaunchParams::CProcessLaunchParams() = default;

	CProcessLaunchParams::CProcessLaunchParams(CProcessLaunchParams const &_From) = default;
	CProcessLaunchParams::CProcessLaunchParams(CProcessLaunchParams &&_From) = default;

	CProcessLaunchParams &CProcessLaunchParams::operator = (CProcessLaunchParams const &_From) = default;
	CProcessLaunchParams &CProcessLaunchParams::operator = (CProcessLaunchParams &&_From) = default;
}
