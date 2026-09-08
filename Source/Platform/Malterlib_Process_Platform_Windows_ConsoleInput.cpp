// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "Malterlib_Process_Platform_Windows_ConsoleInput.h"

#ifndef MOUSE_HWHEELED
#	define MOUSE_HWHEELED 0x0008
#endif

using namespace NMib::NStr;

namespace NMib::NProcess::NPlatform
{
	namespace
	{
		// The xterm modifier parameter: 1 plus the modifier bits. AltGr arrives as right alt with
		// left control and produces a character of its own, so it is neither alt nor control
		uint32 fg_ModifierParam(DWORD _ControlKeyState)
		{
			bool bAltGr = (_ControlKeyState & RIGHT_ALT_PRESSED) && (_ControlKeyState & LEFT_CTRL_PRESSED);

			uint32 Param = 1;
			if (_ControlKeyState & SHIFT_PRESSED)
				Param += 1;
			if (!bAltGr && (_ControlKeyState & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)))
				Param += 2;
			if (!bAltGr && (_ControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)))
				Param += 4;

			return Param;
		}

		bool fg_IsAltPressed(DWORD _ControlKeyState)
		{
			bool bAltGr = (_ControlKeyState & RIGHT_ALT_PRESSED) && (_ControlKeyState & LEFT_CTRL_PRESSED);
			return !bAltGr && (_ControlKeyState & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED));
		}

		bool fg_IsCtrlPressed(DWORD _ControlKeyState)
		{
			bool bAltGr = (_ControlKeyState & RIGHT_ALT_PRESSED) && (_ControlKeyState & LEFT_CTRL_PRESSED);
			return !bAltGr && (_ControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED));
		}

		// The final letter of a cursor key's CSI sequence, 0 for other keys
		char fg_CursorKeyFinal(WORD _VirtualKey)
		{
			switch (_VirtualKey)
			{
				case VK_UP: return 'A';
				case VK_DOWN: return 'B';
				case VK_RIGHT: return 'C';
				case VK_LEFT: return 'D';
				case VK_HOME: return 'H';
				case VK_END: return 'F';
				default: return 0;
			}
		}

		// The number of a tilde key's CSI sequence, 0 for other keys
		uint32 fg_TildeKeyNumber(WORD _VirtualKey)
		{
			switch (_VirtualKey)
			{
				case VK_INSERT: return 2;
				case VK_DELETE: return 3;
				case VK_PRIOR: return 5;
				case VK_NEXT: return 6;
				case VK_F5: return 15;
				case VK_F6: return 17;
				case VK_F7: return 18;
				case VK_F8: return 19;
				case VK_F9: return 20;
				case VK_F10: return 21;
				case VK_F11: return 23;
				case VK_F12: return 24;
				case VK_F13: return 25;
				case VK_F14: return 26;
				case VK_F15: return 28;
				case VK_F16: return 29;
				case VK_F17: return 31;
				case VK_F18: return 32;
				case VK_F19: return 33;
				case VK_F20: return 34;
				default: return 0;
			}
		}

		// The final letter of F1 to F4, which travel as SS3 sequences unmodified, 0 for other keys
		char fg_FunctionKeyFinal(WORD _VirtualKey)
		{
			switch (_VirtualKey)
			{
				case VK_F1: return 'P';
				case VK_F2: return 'Q';
				case VK_F3: return 'R';
				case VK_F4: return 'S';
				default: return 0;
			}
		}

		bool fg_IsModifierKey(WORD _VirtualKey)
		{
			switch (_VirtualKey)
			{
				case VK_SHIFT:
				case VK_LSHIFT:
				case VK_RSHIFT:
				case VK_CONTROL:
				case VK_LCONTROL:
				case VK_RCONTROL:
				case VK_MENU:
				case VK_LMENU:
				case VK_RMENU:
				case VK_LWIN:
				case VK_RWIN:
				case VK_CAPITAL:
				case VK_NUMLOCK:
				case VK_SCROLL:
					return true;

				default:
					return false;
			}
		}

		// The SGR mouse button number of the lowest console button held, or -1 with none held
		int32 fg_MouseButtonNumber(DWORD _ButtonState)
		{
			if (_ButtonState & FROM_LEFT_1ST_BUTTON_PRESSED)
				return 0;
			if (_ButtonState & FROM_LEFT_2ND_BUTTON_PRESSED)
				return 1;
			if (_ButtonState & RIGHTMOST_BUTTON_PRESSED)
				return 2;

			return -1;
		}
	}


	void CWindowsConsoleInputTranslator::fsp_Append(CWStr &o_Text, CStr const &_Sequence)
	{
		o_Text += CWStr(_Sequence);
	}

	// Emits console-compatible VT key sequences when enabled; otherwise emits only text.
	void CWindowsConsoleInputTranslator::f_TranslateKey(KEY_EVENT_RECORD const &_Key, CWStr &o_Text) const
	{
		if (!_Key.bKeyDown || fg_IsModifierKey(_Key.wVirtualKeyCode))
			return;

		ch16 Char = ch16(_Key.uChar.UnicodeChar);
		DWORD State = _Key.dwControlKeyState;

		for (WORD iRepeat = 0; iRepeat < _Key.wRepeatCount; ++iRepeat)
		{
			if (!m_bVirtualTerminalInput)
			{
				if (Char)
					o_Text.f_AddChar(Char);

				continue;
			}

			uint32 Modifiers = fg_ModifierParam(State);

			if (char Final = fg_CursorKeyFinal(_Key.wVirtualKeyCode))
			{
				if (Modifiers == 1)
					fsp_Append(o_Text, CStr::CFormat("\x1B[{}") << fg_CharToString(Final));
				else
					fsp_Append(o_Text, CStr::CFormat("\x1B[1;{}{}") << Modifiers << fg_CharToString(Final));

				continue;
			}

			if (uint32 Number = fg_TildeKeyNumber(_Key.wVirtualKeyCode))
			{
				if (Modifiers == 1)
					fsp_Append(o_Text, CStr::CFormat("\x1B[{}~") << Number);
				else
					fsp_Append(o_Text, CStr::CFormat("\x1B[{};{}~") << Number << Modifiers);

				continue;
			}

			if (char Final = fg_FunctionKeyFinal(_Key.wVirtualKeyCode))
			{
				if (Modifiers == 1)
					fsp_Append(o_Text, CStr::CFormat("\x1BO{}") << fg_CharToString(Final));
				else
					fsp_Append(o_Text, CStr::CFormat("\x1B[1;{}{}") << Modifiers << fg_CharToString(Final));

				continue;
			}

			switch (_Key.wVirtualKeyCode)
			{
				case VK_BACK:
					// DEL for backspace and BS with control, as the console sends them
					if (fg_IsAltPressed(State))
						o_Text.f_AddChar(0x1B);
					o_Text.f_AddChar(fg_IsCtrlPressed(State) ? 0x08 : 0x7F);
					continue;

				case VK_TAB:
					if (State & SHIFT_PRESSED)
						fsp_Append(o_Text, "\x1B[Z");
					else
						o_Text.f_AddChar('\t');
					continue;

				case VK_RETURN:
					if (fg_IsAltPressed(State))
						o_Text.f_AddChar(0x1B);
					o_Text.f_AddChar('\r');
					continue;

				case VK_ESCAPE:
					o_Text.f_AddChar(0x1B);
					continue;

				case VK_SPACE:
					if (fg_IsAltPressed(State))
						o_Text.f_AddChar(0x1B);
					o_Text.f_AddChar(fg_IsCtrlPressed(State) ? 0 : ' ');
					continue;

				default:
					break;
			}

			// The console already supplies control characters; textless key records produce no output.
			if (!Char)
				continue;

			if (fg_IsAltPressed(State))
				o_Text.f_AddChar(0x1B);
			o_Text.f_AddChar(Char);
		}
	}

	// Emit SGR mouse reports only in virtual-terminal input mode.
	// Limit motion to held buttons because the application's selected mouse mode is not tracked.
	void CWindowsConsoleInputTranslator::f_TranslateMouse(MOUSE_EVENT_RECORD const &_Mouse, CWStr &o_Text)
	{
		if (!m_bVirtualTerminalInput)
			return;

		uint32 Modifiers = 0;
		if (_Mouse.dwControlKeyState & SHIFT_PRESSED)
			Modifiers |= 4;
		if (fg_IsAltPressed(_Mouse.dwControlKeyState))
			Modifiers |= 8;
		if (fg_IsCtrlPressed(_Mouse.dwControlKeyState))
			Modifiers |= 16;

		int32 X = int32(_Mouse.dwMousePosition.X) + 1;
		int32 Y = int32(_Mouse.dwMousePosition.Y) + 1;

		auto fReport = [&](uint32 _Button, bool _bRelease)
			{
				fsp_Append(o_Text, CStr::CFormat("\x1B[<{};{};{}{}") << (_Button | Modifiers) << X << Y << (_bRelease ? "m" : "M"));
			}
		;

		DWORD Buttons = _Mouse.dwButtonState & (FROM_LEFT_1ST_BUTTON_PRESSED | FROM_LEFT_2ND_BUTTON_PRESSED | RIGHTMOST_BUTTON_PRESSED);
		int32 WheelDelta = int32(int16(HIWORD(_Mouse.dwButtonState)));

		if (_Mouse.dwEventFlags & MOUSE_WHEELED)
		{
			fReport(WheelDelta > 0 ? 64 : 65, false);
			return;
		}

		if (_Mouse.dwEventFlags & MOUSE_HWHEELED)
		{
			fReport(WheelDelta > 0 ? 67 : 66, false);
			return;
		}

		if (_Mouse.dwEventFlags & MOUSE_MOVED)
		{
			int32 Held = fg_MouseButtonNumber(Buttons);
			if (Held >= 0)
				fReport(uint32(Held) + 32, false);

			return;
		}

		DWORD Changed = Buttons ^ mp_LastMouseButtons;
		mp_LastMouseButtons = Buttons;

		DWORD const ButtonBits[] = {FROM_LEFT_1ST_BUTTON_PRESSED, FROM_LEFT_2ND_BUTTON_PRESSED, RIGHTMOST_BUTTON_PRESSED};
		for (DWORD Bit : ButtonBits)
		{
			if (!(Changed & Bit))
				continue;

			fReport(uint32(fg_MouseButtonNumber(Bit)), (Buttons & Bit) == 0);
		}
	}

}
