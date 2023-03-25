// Copyright © 2016 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>

#include "Malterlib_Process_ProcessLaunch.h"

namespace NMib::NProcess
{
	namespace
	{
		NContainer::TCVector<mint> fg_GetProcesses(NFunction::TCFunction<bool (CProcessInfo const &_ProcessInfo)> const &_fProcessFilter, EProcessInfoFlag _InfoFlags)
		{
			NContainer::TCVector<mint> Return;
			NContainer::TCVector<CProcessInfo> Processes = NPlatform::fg_Process_Enum(_InfoFlags);

			mint ThisProcessID = NPlatform::fg_Process_GetCurrentUID();
			for (auto &Process : Processes)
			{
				if (Process.m_ProcessID == ThisProcessID)
					continue;

				if (!_fProcessFilter(Process))
					continue;

				Return.f_Insert(Process.m_ProcessID);
			}

			return Return;
		}
	}

	mint CProcessLaunch::fs_KillProcesses(NFunction::TCFunction<bool (CProcessInfo const &_ProcessInfo)> const &_fProcessFilter, EProcessInfoFlag _InfoFlags, fp64 _Timeout)
	{
		using namespace NMib::NStr;

		NContainer::TCVector<mint> ProcessIDs = fg_GetProcesses(_fProcessFilter, _InfoFlags);

		NContainer::TCSet<mint> Killed;
		if (!ProcessIDs.f_IsEmpty())
		{
			// Gracefully stop
			NTime::CClock Clock;

#ifndef DPlatformFamily_Windows
			for (auto ProcessID : ProcessIDs)
			{
				try
				{
					Killed[ProcessID];
					NProcess::NPlatform::fg_Process_Stop(ProcessID);
				}
				catch (NException::CException const &)
				{
				}
			}

			// Wait 30 seconds for stop to complete
			Clock.f_Start();
			while (Clock.f_GetTime() < _Timeout)
			{
				ProcessIDs = fg_GetProcesses(_fProcessFilter, _InfoFlags);
				if (ProcessIDs.f_IsEmpty())
					break;
				NSys::fg_Thread_Sleep(0.1f);
			}
#endif
			// If we still have processes, terminate them
			for (auto ProcessID : ProcessIDs)
			{
				try
				{
					Killed[ProcessID];
					NProcess::NPlatform::fg_Process_Terminate(ProcessID);
				}
				catch (NException::CException const &)
				{
				}
			}

			// Wait for terminate to finish
			Clock.f_Start();
			while (Clock.f_GetTime() < _Timeout)
			{
				ProcessIDs = fg_GetProcesses(_fProcessFilter, _InfoFlags);
				if (ProcessIDs.f_IsEmpty())
					break;
				NSys::fg_Thread_Sleep(0.1f);
			}

			// Fail and report failure
			if (!ProcessIDs.f_IsEmpty())
				DMibError("Failed to kill all processes: {vs}"_f << ProcessIDs);
		}
		return Killed.f_GetLen();
	}

	mint CProcessLaunch::fs_KillProcessesInDirectory(NStr::CStr const &_NamePattern, NStr::CStr const &_ArgsPattern, NStr::CStr const &_Directory, fp64 _Timeout)
	{
		NStr::CStr Directory;
		if (_Directory.f_IsEmpty())
			Directory = NFile::CFile::fs_GetProgramDirectory();
		else
			Directory = _Directory;

		if (Directory[Directory.f_GetLen() - 1] != '/')
			Directory += "/";

		return fs_KillProcesses
			(
				[&](CProcessInfo const &_ProcessInfo) -> bool
				{
					if (!_ProcessInfo.m_FullPath.f_StartsWith(Directory))
						return false;

					if
						(
							NStr::fg_StrMatchWildcard
							(
								NFile::CFile::fs_GetFile(_ProcessInfo.m_FullPath).f_GetStr()
								, _NamePattern.f_GetStr()
							)
							!= NStr::EMatchWildcardResult_WholeStringMatchedAndPatternExhausted
						)
					{
						return false;
					}

					if (_ArgsPattern.f_IsEmpty())
						return true;

					if (_ProcessInfo.m_Args.f_IsEmpty())
						return true;

					for (auto const &Arg : _ProcessInfo.m_Args)
					{
						if (NStr::fg_StrMatchWildcard(Arg.f_GetStr(), _ArgsPattern.f_GetStr()) == NStr::EMatchWildcardResult_WholeStringMatchedAndPatternExhausted)
							return true;
					}

					return false;
				}
				, EProcessInfoFlag_FullPath | EProcessInfoFlag_Args
				, _Timeout
			)
		;
	}
}
