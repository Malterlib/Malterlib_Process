// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Test/Exception>
#include <Mib/Process/StdIn>

#ifdef DPlatformFamily_Windows
#include "../Source/Platform/Malterlib_Process_Platform_Windows_ConsoleInput.h"

#ifndef MOUSE_HWHEELED
#	define MOUSE_HWHEELED 0x0008
#endif

namespace
{
	using namespace NMib;
	using namespace NMib::NStr;
	using NProcess::NPlatform::CWindowsConsoleInputTranslator;

	struct CWindowsConsoleInput_Tests : NTest::CTest
	{
		void f_DoTests()
		{
			DMibTestSuite("Keys")
			{
				CWindowsConsoleInputTranslator Translator;
				Translator.m_bVirtualTerminalInput = true;

				auto fCheck = [&](CStr const &_Case, WORD _Key, DWORD _Modifiers, ch16 _Char, CWStr const &_Expected, WORD _Repeats = 1, bool _bDown = true)
					{
						DMibTestPath(_Case);
						KEY_EVENT_RECORD Key{};
						Key.bKeyDown = _bDown;
						Key.wVirtualKeyCode = _Key;
						Key.dwControlKeyState = _Modifiers;
						Key.uChar.UnicodeChar = WCHAR(_Char);
						Key.wRepeatCount = _Repeats;
						CWStr Text;
						Translator.f_TranslateKey(Key, Text);

						DMibExpect(Text, ==, _Expected);
					}
				;

				fCheck("Up", VK_UP, 0, 0, u"\x1B[A");
				fCheck("Down", VK_DOWN, 0, 0, u"\x1B[B");
				fCheck("Right", VK_RIGHT, 0, 0, u"\x1B[C");
				fCheck("Left", VK_LEFT, 0, 0, u"\x1B[D");
				fCheck("Home", VK_HOME, 0, 0, u"\x1B[H");
				fCheck("End", VK_END, 0, 0, u"\x1B[F");
				fCheck("Shift", VK_UP, SHIFT_PRESSED, 0, u"\x1B[1;2A");
				fCheck("Alt", VK_UP, LEFT_ALT_PRESSED, 0, u"\x1B[1;3A");
				fCheck("Control", VK_UP, RIGHT_CTRL_PRESSED, 0, u"\x1B[1;5A");
				fCheck("AllModifiers", VK_UP, SHIFT_PRESSED | LEFT_ALT_PRESSED | LEFT_CTRL_PRESSED, 0, u"\x1B[1;8A");
				fCheck("Insert", VK_INSERT, 0, 0, u"\x1B[2~");
				fCheck("DeleteControl", VK_DELETE, LEFT_CTRL_PRESSED, 0, u"\x1B[3;5~");
				fCheck("PageUp", VK_PRIOR, 0, 0, u"\x1B[5~");
				fCheck("PageDown", VK_NEXT, 0, 0, u"\x1B[6~");
				fCheck("F1", VK_F1, 0, 0, u"\x1BOP");
				fCheck("F4Alt", VK_F4, LEFT_ALT_PRESSED, 0, u"\x1B[1;3S");
				fCheck("F5", VK_F5, 0, 0, u"\x1B[15~");
				fCheck("F12", VK_F12, 0, 0, u"\x1B[24~");
				fCheck("F20", VK_F20, 0, 0, u"\x1B[34~");
				fCheck("Backspace", VK_BACK, 0, 8, u"\x7F");
				fCheck("ControlBackspace", VK_BACK, LEFT_CTRL_PRESSED, 127, u"\x08");
				fCheck("ShiftTab", VK_TAB, SHIFT_PRESSED, '\t', u"\x1B[Z");
				fCheck("AltReturn", VK_RETURN, LEFT_ALT_PRESSED, '\r', u"\x1B\r");
				fCheck("AltText", 'A', LEFT_ALT_PRESSED, 'a', u"\x1B" u"a");
				fCheck("AltGr", 'Q', RIGHT_ALT_PRESSED | LEFT_CTRL_PRESSED, '@', u"@");
				fCheck("UnicodeRepeats", 'A', 0, u'\u00E5', u"\u00E5\u00E5\u00E5", 3);
				fCheck("KeyUp", 'A', 0, 'a', u"", 1, false);
				fCheck("ModifierOnly", VK_SHIFT, SHIFT_PRESSED, 0, u"");
				fCheck("NoText", 'A', 0, 0, u"");

				Translator.m_bVirtualTerminalInput = false;
				fCheck("PlainText", 'A', LEFT_ALT_PRESSED, 'a', u"a");
				fCheck("PlainCursor", VK_UP, 0, 0, u"");

				DMibExpect(umint(NProcess::EStdInReaderFlag_VirtualTerminalInput), ==, umint(4));
			};

			DMibTestSuite("Mouse")
			{
				CWindowsConsoleInputTranslator Translator;
				Translator.m_bVirtualTerminalInput = true;

				auto fCheck = [&](CStr const &_Case, DWORD _Buttons, DWORD _Flags, DWORD _Modifiers, CWStr const &_Expected)
					{
						DMibTestPath(_Case);
						MOUSE_EVENT_RECORD Mouse{};
						Mouse.dwMousePosition = {4, 6};
						Mouse.dwButtonState = _Buttons;
						Mouse.dwEventFlags = _Flags;
						Mouse.dwControlKeyState = _Modifiers;
						CWStr Text;
						Translator.f_TranslateMouse(Mouse, Text);

						DMibExpect(Text, ==, _Expected);
					}
				;

				fCheck("LeftPress", FROM_LEFT_1ST_BUTTON_PRESSED, 0, 0, u"\x1B[<0;5;7M");
				fCheck("Unchanged", FROM_LEFT_1ST_BUTTON_PRESSED, 0, 0, u"");
				fCheck("Drag", FROM_LEFT_1ST_BUTTON_PRESSED, MOUSE_MOVED, SHIFT_PRESSED, u"\x1B[<36;5;7M");
				fCheck("LeftRelease", 0, 0, 0, u"\x1B[<0;5;7m");
				fCheck("Hover", 0, MOUSE_MOVED, 0, u"");
				fCheck("MiddlePress", FROM_LEFT_2ND_BUTTON_PRESSED, 0, 0, u"\x1B[<1;5;7M");
				fCheck("MiddleRelease", 0, 0, 0, u"\x1B[<1;5;7m");
				fCheck("RightModifiers", RIGHTMOST_BUTTON_PRESSED, 0, LEFT_ALT_PRESSED | LEFT_CTRL_PRESSED, u"\x1B[<26;5;7M");
				fCheck("RightRelease", 0, 0, 0, u"\x1B[<2;5;7m");
				fCheck("WheelUp", DWORD(120) << 16, MOUSE_WHEELED, 0, u"\x1B[<64;5;7M");
				fCheck("WheelDown", DWORD(0xFF88) << 16, MOUSE_WHEELED, 0, u"\x1B[<65;5;7M");
				fCheck("WheelRight", DWORD(120) << 16, MOUSE_HWHEELED, 0, u"\x1B[<67;5;7M");
				fCheck("WheelLeft", DWORD(0xFF88) << 16, MOUSE_HWHEELED, 0, u"\x1B[<66;5;7M");

				Translator.m_bVirtualTerminalInput = false;
				fCheck("Disabled", FROM_LEFT_1ST_BUTTON_PRESSED, 0, 0, u"");
			};
		}
	};

	DMibTestRegister(CWindowsConsoleInput_Tests, Malterlib::Process);
}
#endif
