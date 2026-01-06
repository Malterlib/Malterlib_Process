// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

namespace NMib::NProcess::NPlatform
{
	struct CPOSIXProcessInfo
	{
		pid_t m_ProcessID;
		pid_t m_ParentProcessID;
		uint64 m_StartTime;
	};

	NContainer::TCVector<CPOSIXProcessInfo> fg_Posix_ImpSpecefic_EnumProcesses();
}
