// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Windows.h>
#include "../Malterlib_Process_Platform.h"
#include "Malterlib_Process_Platform_Windows_ConsoleInput.h"

#include <Mib/Core/PlatformSpecific/WindowsError>
#include <Mib/Core/PlatformSpecific/WindowsOptional>
#include <Mib/Process/StdIn>

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

	// Subscriptions that outlive the reader are unlinked and no longer receive callbacks.
	struct CWindowsScreenChangeSubscriber
	{
		NFunction::TCFunction<void (NSys::CConsoleProperties const &_ConsoleProperties)> m_fOnScreenChange;
		DMibListLinkDS_Link(CWindowsScreenChangeSubscriber, m_Link);
	};

	// Large chunks amortize system calls and actor deliveries for piped input.
	constexpr umint gc_StdInReadChunkBytes = 64 * 1024;

	// Console records are drained on the owning I/O loop. Pipe input uses a cancellable blocking read on a dedicated thread.
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
				// Cancellation between reads is lost; retry until the read thread observes shutdown.
				f_Stop(false);
				while (f_GetState() >= NThread::EThreadState_Running)
				{
					NLocal::g_OptionalFunctions.m_fCancelSynchronousIo(f_GetThread());
					NLocal::g_OptionalFunctions.m_fCancelIoEx(mp_hStdInFile, nullptr);
					NSys::fg_Thread_SmallestSleep();
				}
				f_Stop(true);
			}

			if (m_bConsoleModeSaved)
				SetConsoleMode(mp_hStdInFile, m_OldConsoleMode);
		}

		// Reapplies the console mode from the union of all registered readers' flags; must be
		// called with the subsystem lock held whenever the reader set changes
		void f_UpdateConsoleMode()
		{
			if (!m_bConsoleModeSaved)
				return;

			mp_Translator.m_bVirtualTerminalInput = false;
			for (auto iReader = m_Readers.f_GetIterator(); iReader; ++iReader)
			{
				if (iReader->m_pParams->m_Flags & EStdInReaderFlag_VirtualTerminalInput)
					mp_Translator.m_bVirtualTerminalInput = true;
			}

			// Raw records bypass line editing and echo. Preserve processed input so Ctrl+C retains the caller's control-event policy.
			DWORD NewConsoleMode = (m_OldConsoleMode & ~DWORD(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT)) | DWORD(ENABLE_WINDOW_INPUT);
			if (mp_Translator.m_bVirtualTerminalInput)
				NewConsoleMode |= ENABLE_MOUSE_INPUT;

			SetConsoleMode(mp_hStdInFile, NewConsoleMode);
		}

		bool f_NeedsIoLoop() const
		{
			return mp_hStdInFile && GetFileType(mp_hStdInFile) == FILE_TYPE_CHAR;
		}

		void f_Init(NSys::ICIoLoop *_pLoop)
		{
			if (!mp_hStdInFile)
				return;

			auto HandleType = GetFileType(mp_hStdInFile);
			if (HandleType == FILE_TYPE_CHAR)
			{
				m_bIsChar = true;

				mp_pLoop = _pLoop;
				if (!mp_pLoop)
					DMibError("No io loop is available to read the console on");

				return;
			}

			if (HandleType == FILE_TYPE_PIPE)
			{
				m_bIsPipe = true;

				return;
			}

			DMibError("Stdin handle is not of a supported type (char or pipe)");
		}

		// Callable from any thread after earlier removals finish. Pipe readers use their own thread and need no registration.
		void f_Register()
		{
			if (m_bIsPipe)
			{
				f_Start();
				return;
			}

			if (!m_bIsChar)
				return;

			DMibFastCheck(!mp_pRegistration);

			m_bConsoleModeSaved = GetConsoleMode(mp_hStdInFile, &m_OldConsoleMode) != 0;
			f_UpdateConsoleMode();

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

		// Transfers the registration to its closer; null when not registered.
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
		DWORD m_OldConsoleMode = 0;
		bool m_bConsoleModeSaved = false;
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
				// Wait for explicit shutdown after EOF or error.
				if (!fp_ReadPipe())
					m_EventWantQuit.f_Wait();
			}

			return 0;
		}

		static void fs_OnConsoleSignaled(void *_pToken, NSys::EIoLoopEvent _Events, int _Error)
		{
			static_cast<CWindowsStdInReaderImplementation *>(_pToken)->fp_OnConsoleSignaled(_Events, _Error);
		}

		void fp_OnConsoleSignaled(NSys::EIoLoopEvent _Events, int _Error);

		// ReadConsoleW can block on textless events, so drain raw records instead.
		// Coalesce text and report the final screen size once per batch.
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
							mp_Translator.f_TranslateKey(Record.Event.KeyEvent, Text);
							break;

						case MOUSE_EVENT:
							mp_Translator.f_TranslateMouse(Record.Event.MouseEvent, Text);
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

		void fp_NotifyScreenChange();

		// Blocking reads on the dedicated reader thread.
		// Returns false after EOF, an error, or shutdown cancellation.
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

		CWindowsConsoleInputTranslator mp_Translator;

		bool mp_bFinished = false; // A console error was reported; do not request further input.
	};

	struct CSubSystem_Process_Platform_Windows_StdInReader : public CSubSystem
	{
		// Runs after removal acknowledgement; registers a pending reader only when no old registrations remain.
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
			DMibFastCheck(m_pStdInReaderImp.f_IsEmpty());
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

		umint m_nClosing = 0; // Outstanding removals; a replacement may register only when this reaches zero.

		NAtomic::TCAtomic<bool> m_bWasDestroyed; // Lets late deregistration callbacks skip registration after subsystem destruction.
		bool m_bRegistrationPending = false;
	};

	constinit TCSubSystem<CSubSystem_Process_Platform_Windows_StdInReader, ESubSystemDestruction_BeforeMemoryManager> g_SubSystem_Process_Platform_Windows_StdInReader = {DAggregateInit};

	CWindowsStdInReader::~CWindowsStdInReader()
	{
		if (!m_Link.f_IsInList())
			return;

		auto &SubSystem = *g_SubSystem_Process_Platform_Windows_StdInReader;
		NStorage::TCUniquePointer<CWindowsStdInReaderImplementation> pToClose;
		NSys::ICIoLoop *pLoop = nullptr;
		NSys::CIoLoopRegistration *pRegistration = nullptr;
		{
			DMibLock(SubSystem.m_StdInReaderImpLock);
			NStorage::TCPointer<CWindowsStdInReaderImplementation> pImp = SubSystem.m_pStdInReaderImp.f_Get();
			m_Link.f_Unlink();
			if (pImp->m_Readers.f_IsEmpty())
			{
				// Unlink subscriptions that can outlive the reader so their scopes remain safe to release.
				while (!pImp->m_ScreenChangeSubscribers.f_IsEmpty())
					pImp->m_ScreenChangeSubscribers.f_GetFirst()->m_Link.f_Unlink();

				pToClose = fg_Move(SubSystem.m_pStdInReaderImp);
				pLoop = pToClose->f_GetLoop();
				pRegistration = pToClose->f_TakeRegistration();
				++SubSystem.m_nClosing;

				SubSystem.m_bRegistrationPending = false;
			}
			else
				pImp->f_UpdateConsoleMode();
		}

		if (!pToClose)
			return;

		// Release the lock before deregistration because delivery takes it.
		// Console state is restored after acknowledgement; pipe-reader destruction stops its read thread.
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

		// Read once per batch so resize bursts report their final dimensions.
		auto ConsoleProperties = NSys::fg_GetConsoleProperties();

		for (auto iSubscriber = m_ScreenChangeSubscribers.f_GetIterator(); iSubscriber; ++iSubscriber)
			iSubscriber->m_fOnScreenChange(ConsoleProperties);
	}

	// Serialize reads and readiness requests with the last reader's registration removal.
	void CWindowsStdInReaderImplementation::fp_OnConsoleSignaled(NSys::EIoLoopEvent _Events, int _Error)
	{
		auto &SubSystem = *g_SubSystem_Process_Platform_Windows_StdInReader;
		DMibLock(SubSystem.m_StdInReaderImpLock);

		if (mp_bFinished || !mp_pRegistration)
			return;

		if (fg_IsSet(_Events, NSys::EIoLoopEvent::mc_Error))
		{
			mp_bFinished = true;

			DWORD Error = _Error ? DWORD(_Error) : DWORD(ERROR_INVALID_HANDLE);
			fp_SendToReaders(EStdInReaderOutputType_GeneralError, "Failed to watch the console input handle: {}"_f << NMib::NPlatform::fg_Win32_GetLastErrorStr(Error));
			return;
		}

		fp_ReadConsole();

		// Draining resets the handle; a record arriving before rearm signals it again.
		if (!mp_bFinished && mp_pRegistration)
			mp_pLoop->f_RequestReadiness(mp_pRegistration, NSys::EIoLoopEvent::mc_Read);
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

	auto *pLoop = NSys::fg_GetThreadIoLoop();
	while (true)
	{
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

			if (!pImp && !pNew)
				pNew = fg_Construct<CWindowsStdInReaderImplementation>();

			if (pImp || pLoop || !pNew->f_NeedsIoLoop())
			{
				if (!pImp)
					pImp = pNew.f_Get();

				pImp->m_Readers.f_Insert(*pReader);

				if (!SubSystem.m_pStdInReaderImp)
				{
					pImp->f_Init(pLoop);
					SubSystem.m_pStdInReaderImp = fg_Move(pNew);

					if (SubSystem.m_nClosing)
						SubSystem.m_bRegistrationPending = true;
					else
						pImp->f_Register();
				}
				else
					pImp->f_UpdateConsoleMode();

				return pReader.f_Detach();
			}
		}

		// Lazy creation registers subsystems. Release the reader lock first, then recheck for a competing registration.
		pLoop = NSys::fg_GetSharedIoLoop();
		if (!pLoop)
			DMibError("No io loop is available to read the console on");
	}
}

void NMib::NProcess::NPlatform::fg_Process_StdInReader_Close(void *_pStdInReader)
{
	NStorage::TCUniquePointer<CWindowsStdInReader> pReader = fg_Explicit((CWindowsStdInReader *)_pStdInReader);
}

// Requires a live input reader; non-console input produces no resize events. Console callbacks run on the reader's I/O-loop thread.
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
