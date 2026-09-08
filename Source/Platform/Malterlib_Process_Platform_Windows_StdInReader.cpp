// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Windows.h>
#include "../Malterlib_Process_Platform.h"

#include <Mib/Core/PlatformSpecific/WindowsError>
#include <Mib/Core/PlatformSpecific/WindowsOptional>
#include <Mib/Process/StdIn>

#ifndef MOUSE_HWHEELED
#	define MOUSE_HWHEELED 0x0008
#endif

using namespace NMib::NStr;

namespace NMib::NProcess::NPlatform
{
	struct CWindowsStdInReader
	{
		CWindowsStdInReader(NMib::NProcess::CStdInReaderParams &&_Params)
			: m_pParams(fg_Construct(fg_Move(_Params)))
		{
		}
		CWindowsStdInReader()
		{
		}
		~CWindowsStdInReader();

		NStorage::TCSharedPointer<NMib::NProcess::CStdInReaderParams> m_pParams;
		DMibListLinkDS_Link(CWindowsStdInReader, m_Link);
	};

	// One screen change subscription. Owned by the scope its registration returned and linked into
	// the console reader's list while both exist; the reader unlinks every subscriber when it closes,
	// so a subscription that outlives the reader simply stops
	struct CWindowsScreenChangeSubscriber
	{
		NFunction::TCFunction<void (NSys::CConsoleProperties const &_ConsoleProperties)> m_fOnScreenChange;
		DMibListLinkDS_Link(CWindowsScreenChangeSubscriber, m_Link);
	};

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

	// Bytes per read of a pipe. A pipe never returns more than it holds, and every chunk costs a
	// system call and a delivery to the readers, which is an actor hop, so anything smaller than
	// the pipe bounds a piped bulk input by that overhead rather than by memory copy
	constexpr umint gc_StdInReadChunkBytes = 64 * 1024;

	// Standard input on Windows. A console is read on an io loop: its input handle is a
	// synchronization object signaled while records are queued, which the loop watches through a
	// wait completion packet, and every pass takes all queued records and translates them itself
	// — ReadConsoleW would block whenever the queue holds only records that produce no text, which
	// a loop thread cannot afford. The translation follows what the console does for a virtual
	// terminal input client, so the consumers see what they always saw: text, the legacy escape
	// sequences for functional keys and SGR mouse reports. It also makes the console reader the one
	// consumer of the queue, which is where a resize arrives, so screen changes are reported from
	// here and from nowhere else.
	//
	// A pipe cannot be waited on, so a piped standard input keeps a thread blocked in ReadFile,
	// which CancelSynchronousIo takes out of the read when the reader closes
	struct CWindowsStdInReaderImplementation : public NMib::NThread::CThread
	{
		CWindowsStdInReaderImplementation()
			: mp_hStdInFile(GetStdHandle(STD_INPUT_HANDLE))
		{
		}
		~CWindowsStdInReaderImplementation()
		{
			DMibFastCheck(!mp_pRegistration);
			DMibFastCheck(m_ScreenChangeSubscribers.f_IsEmpty());

			if (m_bIsPipe)
			{
				// The thread parks in ReadFile with nothing else to wake it, so the read is
				// cancelled until the thread has seen the quit; a cancel that lands between two
				// reads is lost, hence the retry
				f_Stop(false);
				while (f_GetState() >= NThread::EThreadState_Running)
				{
					NLocal::g_OptionalFunctions.m_fCancelSynchronousIo(f_GetThread());
					NLocal::g_OptionalFunctions.m_fCancelIoEx(mp_hStdInFile, nullptr);
					NSys::fg_Thread_SmallestSleep();
				}
				f_Stop(true);
			}

			if (m_bIsChar)
				SetConsoleMode(mp_hStdInFile, m_OldConsoleMode);
		}

		// Reapplies the console mode from the union of all registered readers' flags; must be
		// called with the subsystem lock held whenever the reader set changes
		void f_UpdateConsoleMode()
		{
			if (!m_bIsChar)
				return;

			mp_bVirtualTerminalInput = false;
			for (auto iReader = m_Readers.f_GetIterator(); iReader; ++iReader)
			{
				if (iReader->m_pParams->m_Flags & EStdInReaderFlag_VirtualTerminalInput)
					mp_bVirtualTerminalInput = true;
			}

			// Records are read raw, so line editing and echo are the console's to leave alone;
			// window input is what makes it queue a record when the screen buffer is resized.
			// Processed input stays as the caller had it: with it Ctrl+C is the console's to
			// turn into a control event, and it never reaches the queue
			DWORD NewConsoleMode = (m_OldConsoleMode & ~DWORD(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT)) | DWORD(ENABLE_WINDOW_INPUT);

			SetConsoleMode(mp_hStdInFile, NewConsoleMode);
		}

		void f_Init()
		{
			if (!mp_hStdInFile)
				return;

			auto HandleType = GetFileType(mp_hStdInFile);
			if (HandleType == FILE_TYPE_CHAR)
			{
				GetConsoleMode(mp_hStdInFile, &m_OldConsoleMode);
				m_bIsChar = true;
				f_UpdateConsoleMode();

				mp_pLoop = NSys::fg_GetThreadIoLoop();
				if (!mp_pLoop)
					mp_pLoop = NSys::fg_GetSharedIoLoop();
				if (!mp_pLoop)
					DMibError("No io loop is available to read the console on");

				return;
			}

			if (HandleType == FILE_TYPE_PIPE)
			{
				m_bIsPipe = true;
				f_Start();

				return;
			}

			DMibError("Stdin handle is not of a supported type (char or pipe)");
		}

		// Puts the console on the loop; callable from any thread, so the subsystem can issue it
		// from a previous removal's continuation. A pipe reader has nothing to register: its
		// thread is already reading
		void f_Register()
		{
			if (!m_bIsChar)
				return;

			DMibFastCheck(!mp_pRegistration);

			NSys::CIoLoopRegisterOptions Options;
			Options.m_bWaitableHandle = true;

			mp_pRegistration = mp_pLoop->f_Register
				(
					(NSys::CIoLoopHandle)(umint)mp_hStdInFile
					, this
					, NSys::EIoLoopEvent::mc_Read
					, &fs_OnConsoleSignaled
					, false
					, Options
				)
			;
		}

		// Hands the registration to whoever removes it, null when none was issued
		NSys::CIoLoopRegistration *f_TakeRegistration()
		{
			auto *pRegistration = mp_pRegistration;
			mp_pRegistration = nullptr;

			return pRegistration;
		}

		NSys::ICIoLoop *f_GetLoop() const
		{
			return mp_pLoop;
		}

		DMibListLinkDS_List(CWindowsStdInReader, m_Link) m_Readers;
		DMibListLinkDS_List(CWindowsScreenChangeSubscriber, m_Link) m_ScreenChangeSubscribers;
		DWORD m_OldConsoleMode;
		bool m_bIsPipe = false;
		bool m_bIsChar = false;

	private:
		NMib::NStr::CStr f_GetThreadName() override
		{
			return "Stdin reader";
		}

		aint f_Main() override
		{
			while (f_GetState() != NMib::NThread::EThreadState_EventWantQuit)
			{
				// End of file or an error was reported; nothing more is read, and the thread
				// waits to be stopped
				if (!fp_ReadPipe())
					m_EventWantQuit.f_Wait();
			}

			return 0;
		}

		static void fs_OnConsoleSignaled(void *_pToken, NSys::EIoLoopEvent _Events, int _Error)
		{
			static_cast<CWindowsStdInReaderImplementation *>(_pToken)->fp_OnConsoleSignaled(_Events, _Error);
		}

		void fp_OnConsoleSignaled(NSys::EIoLoopEvent _Events, int _Error)
		{
			if (mp_bFinished)
				return;

			if (fg_IsSet(_Events, NSys::EIoLoopEvent::mc_Error))
			{
				// A loop that could not watch the handle says so once; nothing follows
				mp_bFinished = true;

				DWORD Error = _Error ? DWORD(_Error) : DWORD(ERROR_INVALID_HANDLE);
				fp_SendToReaders(EStdInReaderOutputType_GeneralError, "Failed to watch the console input handle: {}"_f << NMib::NPlatform::fg_Win32_GetLastErrorStr(Error));
				return;
			}

			fp_ReadConsole();

			// The queue was emptied, which resets the handle: the would-block observation the
			// request rests on. A record that arrived since signals it again, level at arm
			if (!mp_bFinished)
				mp_pLoop->f_RequestReadiness(mp_pRegistration, NSys::EIoLoopEvent::mc_Read);
		}

		// Takes every queued record and delivers what they mean: the text in one delivery, then
		// one screen change for however many resize records were among them
		void fp_ReadConsole()
		{
			CWStr Text;
			bool bScreenChanged = false;

			while (true)
			{
				DWORD nAvailable = 0;
				if (!GetNumberOfConsoleInputEvents(mp_hStdInFile, &nAvailable))
				{
					mp_bFinished = true;
					fp_SendToReaders(EStdInReaderOutputType_GeneralError, "GetNumberOfConsoleInputEvents failed: {}"_f << NMib::NPlatform::fg_Win32_GetLastErrorStr(GetLastError()));
					break;
				}

				if (!nAvailable)
					break;

				mp_Records.f_SetLen(nAvailable);

				DWORD nRead = 0;
				if (!ReadConsoleInputW(mp_hStdInFile, mp_Records.f_GetArray(), nAvailable, &nRead))
				{
					mp_bFinished = true;
					fp_SendToReaders(EStdInReaderOutputType_GeneralError, "ReadConsoleInputW failed: {}"_f << NMib::NPlatform::fg_Win32_GetLastErrorStr(GetLastError()));
					break;
				}

				for (DWORD i = 0; i < nRead; ++i)
				{
					auto const &Record = mp_Records[i];
					switch (Record.EventType)
					{
						case KEY_EVENT:
							fp_TranslateKey(Record.Event.KeyEvent, Text);
							break;

						case MOUSE_EVENT:
							fp_TranslateMouse(Record.Event.MouseEvent, Text);
							break;

						case WINDOW_BUFFER_SIZE_EVENT:
							bScreenChanged = true;
							break;

						default:
							break;
					}
				}
			}

			if (!Text.f_IsEmpty())
			{
				CStrIO Utf8(Text);
				NContainer::CIOByteVector Bytes;
				Bytes.f_Insert((uint8 const *)Utf8.f_GetStr(), Utf8.f_GetLen());
				fp_SendToReaders(EStdInReaderOutputType_StdIn, Bytes);
			}

			if (bScreenChanged)
				fp_NotifyScreenChange();
		}

		static void fs_Append(CWStr &_Text, CStr const &_Sequence)
		{
			_Text += CWStr(_Sequence);
		}

		// One key record as the console would have translated it for a virtual terminal input
		// client: the key's own character, ESC prefixed under alt; cursor keys as CSI sequences,
		// the tilde keys and F5 up as numbered ones, F1 to F4 as SS3, all with the xterm modifier
		// parameter when a modifier is held. Without virtual terminal input only the characters
		// are delivered
		void fp_TranslateKey(KEY_EVENT_RECORD const &_Key, CWStr &_Text)
		{
			if (!_Key.bKeyDown || fg_IsModifierKey(_Key.wVirtualKeyCode))
				return;

			ch16 Char = ch16(_Key.uChar.UnicodeChar);
			DWORD State = _Key.dwControlKeyState;

			for (WORD iRepeat = 0; iRepeat < _Key.wRepeatCount; ++iRepeat)
			{
				if (!mp_bVirtualTerminalInput)
				{
					if (Char)
						_Text.f_AddChar(Char);

					continue;
				}

				uint32 Modifiers = fg_ModifierParam(State);

				if (char Final = fg_CursorKeyFinal(_Key.wVirtualKeyCode))
				{
					if (Modifiers == 1)
						fs_Append(_Text, CStr::CFormat("\x1B[{}") << fg_CharToString(Final));
					else
						fs_Append(_Text, CStr::CFormat("\x1B[1;{}{}") << Modifiers << fg_CharToString(Final));

					continue;
				}

				if (uint32 Number = fg_TildeKeyNumber(_Key.wVirtualKeyCode))
				{
					if (Modifiers == 1)
						fs_Append(_Text, CStr::CFormat("\x1B[{}~") << Number);
					else
						fs_Append(_Text, CStr::CFormat("\x1B[{};{}~") << Number << Modifiers);

					continue;
				}

				if (char Final = fg_FunctionKeyFinal(_Key.wVirtualKeyCode))
				{
					if (Modifiers == 1)
						fs_Append(_Text, CStr::CFormat("\x1BO{}") << fg_CharToString(Final));
					else
						fs_Append(_Text, CStr::CFormat("\x1B[1;{}{}") << Modifiers << fg_CharToString(Final));

					continue;
				}

				switch (_Key.wVirtualKeyCode)
				{
					case VK_BACK:
						// DEL for backspace and BS with control, as the console sends them
						if (fg_IsAltPressed(State))
							_Text.f_AddChar(0x1B);
						_Text.f_AddChar(fg_IsCtrlPressed(State) ? 0x08 : 0x7F);
						continue;

					case VK_TAB:
						if (State & SHIFT_PRESSED)
							fs_Append(_Text, "\x1B[Z");
						else
							_Text.f_AddChar('\t');
						continue;

					case VK_RETURN:
						if (fg_IsAltPressed(State))
							_Text.f_AddChar(0x1B);
						_Text.f_AddChar('\r');
						continue;

					case VK_ESCAPE:
						_Text.f_AddChar(0x1B);
						continue;

					case VK_SPACE:
						if (fg_IsAltPressed(State))
							_Text.f_AddChar(0x1B);
						_Text.f_AddChar(fg_IsCtrlPressed(State) ? 0 : ' ');
						continue;

					default:
						break;
				}

				// Everything else is its character, which for control combinations the console
				// already resolved to the control character. A key with none — a dead key, an
				// unmapped function key — produces nothing, the same as the console
				if (!Char)
					continue;

				if (fg_IsAltPressed(State))
					_Text.f_AddChar(0x1B);
				_Text.f_AddChar(Char);
			}
		}

		// One mouse record as an SGR report. Mouse records only reach the queue when the console
		// has mouse input enabled and quick edit off, which an application that wants them has
		// arranged, so they are reported whenever virtual terminal input is on. Motion is reported
		// only while a button is held: which of the motion modes the application enabled is not
		// known here, and motion with a button is what every one of them reports
		void fp_TranslateMouse(MOUSE_EVENT_RECORD const &_Mouse, CWStr &_Text)
		{
			if (!mp_bVirtualTerminalInput)
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
					fs_Append(_Text, CStr::CFormat("\x1B[<{};{};{}{}") << (_Button | Modifiers) << X << Y << (_bRelease ? "m" : "M"));
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

			// A press or a release, possibly several at once: each button whose state changed
			// is reported on its own
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

		void fp_NotifyScreenChange();

		// Blocking reads, on the thread, until a short read; false at end of file, on an error or
		// when the read was cancelled by the reader closing, all of which stop the reading
		bool fp_ReadPipe()
		{
			bool bRet = true;

			if (mp_StdInReadBuffer.f_IsEmpty())
				mp_StdInReadBuffer.f_SetLen(gc_StdInReadChunkBytes);

			while (true)
			{
				DWORD nBytesRead = 0;
				if (!ReadFile(mp_hStdInFile, mp_StdInReadBuffer.f_GetArray(), gc_StdInReadChunkBytes, &nBytesRead, nullptr))
				{
					DWORD dwError = GetLastError();

					if (dwError == ERROR_OPERATION_ABORTED)
						return false; // The reader is closing; nobody is left to tell

					if (dwError == ERROR_BROKEN_PIPE || dwError == ERROR_NO_DATA || dwError == ERROR_HANDLE_EOF)
						fp_SendToReaders(EStdInReaderOutputType_EndOfFile, "End of file");
					else
						fp_SendToReaders(EStdInReaderOutputType_GeneralError, "ReadFile failed: {}"_f << NMib::NPlatform::fg_Win32_GetLastErrorStr(dwError));
					bRet = false;
				}
				else
				{
					if (nBytesRead)
						fp_SendToReaders(EStdInReaderOutputType_StdIn, NContainer::CIOByteVector(mp_StdInReadBuffer.f_GetArray(), nBytesRead));
					else
					{
						// nBytesRead == 0 also means EOF
						fp_SendToReaders(EStdInReaderOutputType_EndOfFile, "End of file");
						bRet = false;
					}
				}

				if (nBytesRead != gc_StdInReadChunkBytes)
					break;
			}

			return bRet;
		}

		void fp_SendToReaders(EStdInReaderOutputType _Type, NStr::CStrIO const &_String);
		void fp_SendToReaders(NMib::NProcess::EStdInReaderOutputType _Type, NContainer::CIOByteVector const &_Buffer);

		HANDLE mp_hStdInFile;
		NSys::ICIoLoop *mp_pLoop = nullptr;
		NSys::CIoLoopRegistration *mp_pRegistration = nullptr;
		NContainer::CIOByteVector mp_StdInReadBuffer;
		NContainer::TCVector<INPUT_RECORD> mp_Records;

		// The console buttons held after the last mouse record, for telling presses from releases
		DWORD mp_LastMouseButtons = 0;

		// Any reader asked for virtual terminal input, so functional keys and mouse records are
		// translated to escape sequences; otherwise only the text of the keys is delivered, which
		// is what a raw ReadConsoleW gave
		bool mp_bVirtualTerminalInput = false;

		// An error has been reported from the console; nothing further is read or asked for
		bool mp_bFinished = false;
	};

	struct CSubSystem_Process_Platform_Windows_StdInReader : public CSubSystem
	{
		// Runs a removal's acknowledgement: the implementation is gone, and an implementation that
		// opened meanwhile gets its registration if this was the last removal in the way
		void f_OnClosed()
		{
			if (m_bWasDestroyed.f_Load())
				return;

			DMibLock(m_StdInReaderImpLock);
			--m_nClosing;
			if (m_nClosing || !m_bRegistrationPending)
				return;

			m_bRegistrationPending = false;
			m_pStdInReaderImp->f_Register();
		}

		void f_DestroyThreadSpecific() override
		{
			DMibLock(m_StdInReaderImpLock);
			DMibFastCheck(m_pStdInReaderImp.f_IsEmpty()); // Should have been deleted when the last std in reader was closed
			m_pStdInReaderImp.f_Clear();
		}

		~CSubSystem_Process_Platform_Windows_StdInReader()
		{
			{
				DMibLock(m_StdInReaderImpLock);
				DMibFastCheck(m_pStdInReaderImp.f_IsEmpty());
			}

			m_bWasDestroyed.f_Store(true);
		}

		NThread::CMutual m_StdInReaderImpLock;
		NMib::NStorage::TCUniquePointer<CWindowsStdInReaderImplementation> m_pStdInReaderImp;

		// Implementations taken off the loop and not yet acknowledged. The console can only be
		// registered once, so while any is counted here the live implementation waits for its
		// registration, which the last acknowledgement issues. Nothing ever waits for an
		// acknowledgement: a reader is closed from actor threads, which may not block, and the
		// acknowledgement of a removal started inside the loop's own callback could only come
		// after that callback returns
		umint m_nClosing = 0;

		// Set by the destructor for a continuation that runs after it, when the shared loop drains
		// on its way out; there is nothing left to register by then
		NAtomic::TCAtomic<bool> m_bWasDestroyed;
		bool m_bRegistrationPending = false;
	};

	constinit TCSubSystem<CSubSystem_Process_Platform_Windows_StdInReader, ESubSystemDestruction_BeforeMemoryManager> g_SubSystem_Process_Platform_Windows_StdInReader = {DAggregateInit};

	CWindowsStdInReader::~CWindowsStdInReader()
	{
		if (!m_Link.f_IsInList())
			return;

		auto &SubSystem = *g_SubSystem_Process_Platform_Windows_StdInReader;
		NStorage::TCUniquePointer<CWindowsStdInReaderImplementation> pToClose;
		{
			DMibLock(SubSystem.m_StdInReaderImpLock);
			NStorage::TCPointer<CWindowsStdInReaderImplementation> pImp = SubSystem.m_pStdInReaderImp.f_Get();
			m_Link.f_Unlink();
			if (pImp->m_Readers.f_IsEmpty())
			{
				// Screen change subscriptions stop with the reader they observed through; their
				// scopes find themselves unlinked and do nothing more
				while (!pImp->m_ScreenChangeSubscribers.f_IsEmpty())
					pImp->m_ScreenChangeSubscribers.f_GetFirst()->m_Link.f_Unlink();

				pToClose = fg_Move(SubSystem.m_pStdInReaderImp);
				++SubSystem.m_nClosing;

				// Never registered: it was itself waiting for a removal, and stops waiting
				SubSystem.m_bRegistrationPending = false;
			}
			else
				pImp->f_UpdateConsoleMode();
		}

		if (!pToClose)
			return;

		// Outside the lock, since the loop's callback takes it to deliver. The removal never
		// blocks: the implementation is destroyed, and the console mode restored, once the loop
		// reports that no callback can be in flight, on the loop's own thread. A pipe reader has
		// no registration and is destroyed right here, which stops its thread
		auto *pLoop = pToClose->f_GetLoop();
		auto *pRegistration = pToClose->f_TakeRegistration();
		auto fOnClosed = [pToClose = fg_Move(pToClose)]() mutable
			{
				pToClose.f_Clear();
				g_SubSystem_Process_Platform_Windows_StdInReader->f_OnClosed();
			}
		;

		if (pRegistration)
			pLoop->f_DeregisterAsync(pRegistration, fg_Move(fOnClosed));
		else
			fOnClosed();
	}

	void CWindowsStdInReaderImplementation::fp_NotifyScreenChange()
	{
		auto &SubSystem = *g_SubSystem_Process_Platform_Windows_StdInReader;
		DMibLock(SubSystem.m_StdInReaderImpLock);

		if (m_ScreenChangeSubscribers.f_IsEmpty())
			return;

		// Read once the burst has been consumed, so a run of resize records reports the final size
		auto ConsoleProperties = NSys::fg_GetConsoleProperties();

		for (auto iSubscriber = m_ScreenChangeSubscribers.f_GetIterator(); iSubscriber; ++iSubscriber)
			iSubscriber->m_fOnScreenChange(ConsoleProperties);
	}

	void CWindowsStdInReaderImplementation::fp_SendToReaders(EStdInReaderOutputType _Type, NContainer::CIOByteVector const &_Buffer)
	{
		DMibRequire(_Type == EStdInReaderOutputType_StdIn);
		auto &SubSystem = *g_SubSystem_Process_Platform_Windows_StdInReader;
		DMibLock(SubSystem.m_StdInReaderImpLock);
		for (auto iReader = m_Readers.f_GetIterator(); iReader; ++iReader)
		{
			auto pParams = iReader->m_pParams;
			if (pParams->m_fDispatcher)
			{
				if (pParams->m_fOnReceiveInput)
				{
					pParams->m_fDispatcher
						(
							[_Type, String = NStr::CStrIO(_Buffer.f_GetArray(), _Buffer.f_GetLen()), pParams]()
							{
								pParams->m_fOnReceiveInput(_Type, String);
							}
						)
					;
				}
				else
				{
					pParams->m_fDispatcher
						(
							[_Type, _Buffer, pParams]()
							{
								pParams->m_fOnReceiveBinaryInput(_Type, _Buffer, {});
							}
						)
					;
				}

			}
			else
			{
				if (pParams->m_fOnReceiveInput)
					pParams->m_fOnReceiveInput(_Type, NStr::CStrIO(_Buffer.f_GetArray(), _Buffer.f_GetLen()));
				else
					pParams->m_fOnReceiveBinaryInput(_Type, _Buffer, {});
			}
		}
	}

	void CWindowsStdInReaderImplementation::fp_SendToReaders(EStdInReaderOutputType _Type, NStr::CStrIO const &_String)
	{
		DMibRequire(_Type != EStdInReaderOutputType_StdIn);
		auto &SubSystem = *g_SubSystem_Process_Platform_Windows_StdInReader;
		DMibLock(SubSystem.m_StdInReaderImpLock);
		for (auto iReader = m_Readers.f_GetIterator(); iReader; ++iReader)
		{
			auto pParams = iReader->m_pParams;
			if (pParams->m_fDispatcher)
			{
				if (pParams->m_fOnReceiveInput)
				{
					pParams->m_fDispatcher
						(
							[_Type, _String, pParams]()
							{
								pParams->m_fOnReceiveInput(_Type, _String);
							}
						)
					;
				}
				else
				{
					pParams->m_fDispatcher
						(
							[_Type, _String, pParams]()
							{
								pParams->m_fOnReceiveBinaryInput(_Type, {}, _String);
							}
						)
					;
				}
			}
			else
			{
				if (pParams->m_fOnReceiveInput)
					pParams->m_fOnReceiveInput(_Type, _String);
				else
					pParams->m_fOnReceiveBinaryInput(_Type, {}, _String);
			}
		}
	}
}

void *NMib::NProcess::NPlatform::fg_Process_StdInReader_Open(NMib::NProcess::CStdInReaderParams &&_Params)
{
	NStorage::TCUniquePointer<CWindowsStdInReaderImplementation> pNew; // First because we want it to be destroyed after pReader in case of exception in f_Init
	NStorage::TCUniquePointer<CWindowsStdInReader> pReader = fg_Construct(fg_Move(_Params));

	auto &Params = *pReader->m_pParams;

	if (Params.m_fOnReceiveInput.f_IsEmpty() && Params.m_fOnReceiveBinaryInput.f_IsEmpty())
		DMibError("No on receive input function specified for stdin reader");

	if (Params.m_fOnReceiveInput.f_IsEmpty() == Params.m_fOnReceiveBinaryInput.f_IsEmpty())
		DMibError("Both string and binary on receive input function specified for stdin reader");

	auto &SubSystem = *g_SubSystem_Process_Platform_Windows_StdInReader;

	{
		DMibLock(SubSystem.m_StdInReaderImpLock);

		NStorage::TCPointer<CWindowsStdInReaderImplementation> pImp = SubSystem.m_pStdInReaderImp.f_Get();

		if (Params.m_Flags & EStdInReaderFlag_Exclusive)
		{
			if (pImp && !pImp->m_Readers.f_IsEmpty())
				DMibError("Stdin reader opened for exclusive access, but another reader is already open");
		}
		else
		{
			if (pImp && !pImp->m_Readers.f_IsEmpty() && (pImp->m_Readers.f_GetFirst()->m_pParams->m_Flags & EStdInReaderFlag_Exclusive))
				DMibError("There is already a stdin reader opened for exclusive access");
		}

		if (!pImp)
		{
			pNew = fg_Construct<CWindowsStdInReaderImplementation>();
			pImp = (CWindowsStdInReaderImplementation *)pNew.f_Get();
		}

		pImp->m_Readers.f_Insert(*pReader);

		if (!SubSystem.m_pStdInReaderImp)
		{
			pImp->f_Init();
			SubSystem.m_pStdInReaderImp = fg_Move(pNew);

			// A previous implementation still on its way off the loop registers this one when
			// its removal is acknowledged; input is delayed by that round trip and nothing waits
			if (SubSystem.m_nClosing)
				SubSystem.m_bRegistrationPending = true;
			else
				pImp->f_Register();
		}
		else
			pImp->f_UpdateConsoleMode();
	}

	return pReader.f_Detach();
}

void NMib::NProcess::NPlatform::fg_Process_StdInReader_Close(void *_pStdInReader)
{
	NStorage::TCUniquePointer<CWindowsStdInReader> pReader = fg_Explicit((CWindowsStdInReader *)_pStdInReader);
}

auto NMib::NProcess::NPlatform::fg_Process_StdInReader_RegisterScreenChange
	(
		NFunction::TCFunction<void (NSys::CConsoleProperties const &_ConsoleProperties)> &&_fOnScreenChange
	)
	-> NMib::COnScopeExitShared
{
	auto &SubSystem = *g_SubSystem_Process_Platform_Windows_StdInReader;

	NStorage::TCSharedPointer<CWindowsScreenChangeSubscriber> pSubscriber = fg_Construct();
	pSubscriber->m_fOnScreenChange = fg_Move(_fOnScreenChange);

	{
		DMibLock(SubSystem.m_StdInReaderImpLock);

		NStorage::TCPointer<CWindowsStdInReaderImplementation> pImp = SubSystem.m_pStdInReaderImp.f_Get();
		if (!pImp)
			DMibError("Screen change notifications require an open standard input reader");
		if (!pImp->m_bIsChar)
			DMibError("Screen change notifications require standard input to be a console");

		pImp->m_ScreenChangeSubscribers.f_Insert(*pSubscriber);
	}

	return g_OnScopeExitShared / [pSubscriber]
		{
			auto &SubSystem = *g_SubSystem_Process_Platform_Windows_StdInReader;
			DMibLock(SubSystem.m_StdInReaderImpLock);
			if (pSubscriber->m_Link.f_IsInList())
				pSubscriber->m_Link.f_Unlink();
		}
	;
}
