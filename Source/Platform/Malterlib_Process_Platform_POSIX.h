// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <sys/types.h>

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
