// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Process/ProcessLaunch>

namespace NMib::NProcess::NPlatform
{
	uint32 fg_Win32_TranslateProcessPriority(EExecutionPriority _Priority);

	class CProcessEntry
	{
	public:
		~CProcessEntry();
		NStr::CStr m_FileName;
		uint32 m_Process;
		NTime::CTime m_CreationTime;
		DMibListLinkDS_Link(CProcessEntry, m_Link);
		DMibListLinkDS_List(CProcessEntry, m_Link) m_Children;

		DMibListLinkDS_Link(CProcessEntry, m_LinkAll);
		DMibListLinkDS_List(CProcessEntry, m_LinkAll) m_AllProcess;

		bint operator == (uint32 _Process) const;
		void f_MapProcess(uint32 _ID, uint32 _ParentID, NStr::CWStr _FileName, const NTime::CTime &_CreationTime);
		void f_KillTree(NStr::CStr &_Log, aint _Depth = 0);
	};

}
