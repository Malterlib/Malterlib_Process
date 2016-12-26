// Copyright © 2016 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>

#include "Malterlib_Process_ProcessLaunch.h"

namespace NMib
{
	namespace NProcess
	{
		namespace
		{
			NContainer::TCVector<mint> fg_GetProcessesMatching(NStr::CStr const &_NamePattern, NStr::CStr const &_ArgsPattern, NStr::CStr const &_Directory)
			{
				NContainer::TCVector<mint> Return;
				NContainer::TCVector<NProcess::CProcessInfo> Processes = NProcess::NPlatform::fg_Process_Enum(NProcess::EProcessInfoFlag_FullPath | NProcess::EProcessInfoFlag_Args);

				mint ThisProcessID = NProcess::NPlatform::fg_Process_GetCurrentUID();
				for (auto &Process : Processes)
				{
					if (Process.m_ProcessID == ThisProcessID)
						continue;

					if (!Process.m_FullPath.f_StartsWith(_Directory))
						continue;

					if (NStr::fg_StrMatchWildcard(NFile::CFile::fs_GetFile(Process.m_FullPath).f_GetStr(), _NamePattern.f_GetStr()) != NStr::EMatchWildcardResult_WholeStringMatchedAndPatternExhausted)
						continue;

					if (_ArgsPattern.f_IsEmpty())
					{
						Return.f_Insert(Process.m_ProcessID);
						continue;
					}

					for (auto const &Arg : Process.m_Args)
					{
						if (NStr::fg_StrMatchWildcard(Arg.f_GetStr(), _ArgsPattern.f_GetStr()) == NStr::EMatchWildcardResult_WholeStringMatchedAndPatternExhausted)
						{
							Return.f_Insert(Process.m_ProcessID);
							break;
						}
					}
				}

				return Return;
			}
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
			
			NContainer::TCVector<mint> ProcessIDs = fg_GetProcessesMatching(_NamePattern, _ArgsPattern, Directory);

			NContainer::TCSet<mint> Killed;
			if (!ProcessIDs.f_IsEmpty())
			{
				// Gracefully stop
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
				NTime::CClock Clock;
				Clock.f_Start();
				while (Clock.f_GetTime() < _Timeout)
				{
					ProcessIDs = fg_GetProcessesMatching(_NamePattern, _ArgsPattern, Directory);
					if (ProcessIDs.f_IsEmpty())
						break;
					NSys::fg_Thread_Sleep(0.1f);
				}

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
					ProcessIDs = fg_GetProcessesMatching(_NamePattern, _ArgsPattern, Directory);
					if (ProcessIDs.f_IsEmpty())
						break;
					NSys::fg_Thread_Sleep(0.1f);
				}

				// Fail and report failure
				if (!ProcessIDs.f_IsEmpty())
				{
					DMibError(fg_Format("Failed to kill processes matching: {} {}", _NamePattern, _ArgsPattern));
				}
			}
			return Killed.f_GetLen();
		}
	}
}
