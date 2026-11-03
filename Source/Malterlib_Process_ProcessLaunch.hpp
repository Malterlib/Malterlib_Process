// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

namespace NMib::NProcess
{
	template <typename tf_CStream>
	void CProcessLimit::f_Feed(tf_CStream &_Stream) const
	{
		_Stream << m_MaxValue;
		_Stream << m_Value;
	}

	template <typename tf_CStream>
	void CProcessLimit::f_Consume(tf_CStream &_Stream)
	{
		_Stream >> m_MaxValue;
		_Stream >> m_Value;
	}

	template <typename tf_CStream>
	void CProcessLaunchParams::f_Feed(tf_CStream &_Stream) const
	{
		_Stream << EProtocolVersion_Current;
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
		Temp = m_bAllowLaunchedInForeground;
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
		Temp = m_bLaunchInUserSession;
		_Stream << Temp;

		_Stream << m_CPUUsage;
		_Stream << m_Elevation;

		_Stream << m_RunAsUser;
		_Stream << m_RunAsUserPassword;
		_Stream << m_RunAsGroup;
	}

	template <typename tf_CStream>
	void CProcessLaunchParams::f_Consume(tf_CStream &_Stream)
	{
		uint32 Version;
		_Stream >> Version;
		if (Version > EProtocolVersion_Current || Version < 0x101)
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
		m_bAllowLaunchedInForeground = Temp;
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

		if (Version >= 0x108)
		{
			_Stream >> Temp;
			m_bLaunchInUserSession = Temp;
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
}
