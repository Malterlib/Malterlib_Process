// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Core/Core>
#include <Mib/Process/ProcessLaunch>

DMibAppNoClass;
DMibPMain;

extern "C"
{
	module_export uint32 calling_convention_c fg_Launch(ch8 const *_pProgram, ch8 const *_pParams)
	{
		uint32 ExitCode = 66;
		NMib::NProcess::CProcessLaunchParams Params;
		Params.m_Target = NMib::NStr::CStr(_pProgram);
		Params.m_Parameters = NMib::NStr::CStr(_pParams);

		Params.m_fOnStateChange
			= [&](NMib::NProcess::CProcessLaunchStateChangeVariant const &_State, fp64 _TimeSinceStart)
			{
				switch (_State.f_GetTypeID())
				{
				case NMib::NProcess::EProcessLaunchState_Exited:
					{
						NMib::fg_Volatile(ExitCode) = _State.f_Get<NMib::NProcess::EProcessLaunchState_Exited>();
					}
					break;
				case NMib::NProcess::EProcessLaunchState_Launched:
				case NMib::NProcess::EProcessLaunchState_LaunchFailed:
					break;
				}
			}
		;

		Params.m_fOnOutput
			= [&](NMib::NProcess::EProcessLaunchOutputType _OutputType, NMib::NStr::CStr const &_Output)
			{
			}
		;
		{
			NMib::NProcess::CProcessLaunch Launcher(Params, NMib::NProcess::EProcessLaunchCloseFlag_BlockOnExit);
		}

		return ExitCode;
	}
}

