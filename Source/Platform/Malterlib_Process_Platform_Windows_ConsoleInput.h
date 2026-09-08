// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <Windows.h>
#include <Mib/String/String>

namespace NMib::NProcess::NPlatform
{
	struct CWindowsConsoleInputTranslator
	{
		void f_TranslateKey(KEY_EVENT_RECORD const &_Key, NStr::CWStr &o_Text) const;
		void f_TranslateMouse(MOUSE_EVENT_RECORD const &_Mouse, NStr::CWStr &o_Text);

		bool m_bVirtualTerminalInput = false;

	private:
		static void fsp_Append(NStr::CWStr &o_Text, NStr::CStr const &_Sequence);

		DWORD mp_LastMouseButtons = 0; // Buttons held after the previous record, used to distinguish presses from releases.
	};
}
