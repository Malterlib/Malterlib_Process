// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Windows.h>
#include "../Malterlib_Process_Platform.h"

#include <Mib/Core/PlatformSpecific/WindowsError>
#include <Mib/Core/PlatformSpecific/WindowsOptional>
#include <Mib/Process/StdIn>
#include <Mib/Time/TimeMeasure>

#ifndef ENABLE_VIRTUAL_TERMINAL_INPUT
#	define ENABLE_VIRTUAL_TERMINAL_INPUT 0x0200
#endif

using namespace NMib::NStr;

namespace NMib::NProcess::NPlatform
{
	struct CWindowsStdInReader
	{
		NStorage::TCSharedPointer<NMib::NProcess::CStdInReaderParams> m_pParams;
		DMibListLinkDS_Link(CWindowsStdInReader, m_Link);

		CWindowsStdInReader(NMib::NProcess::CStdInReaderParams &&_Params)
			: m_pParams(fg_Construct(fg_Move(_Params)))
		{
		}
		CWindowsStdInReader()
		{
		}
		~CWindowsStdInReader();
	};

	struct CWindowsStdInReaderImplementation : public NMib::NThread::CThread
	{
		DMibListLinkDS_List(CWindowsStdInReader, m_Link) m_Readers;

		CWindowsStdInReaderImplementation(bool _bForcePolling)
			: mp_hStdInFile(nullptr)
		{
			if (_bForcePolling)
				m_bDoPolling = true;
			else
				m_bDoPolling = NLocal::g_OptionalFunctions.m_fCancelSynchronousIo == nullptr || NLocal::g_OptionalFunctions.m_fCancelIoEx == nullptr;
			mp_hStdInFile = GetStdHandle(STD_INPUT_HANDLE);
		}
		~CWindowsStdInReaderImplementation()
		{
			if (m_bDoPolling)
				f_Stop(true);
			else
			{
				f_Stop(false);
				NLocal::g_OptionalFunctions.m_fCancelSynchronousIo(f_GetThread());
				NLocal::g_OptionalFunctions.m_fCancelIoEx(mp_hStdInFile, nullptr);
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

		bool m_bIsPipe = false;
		bool m_bIsChar = false;
		bool m_bDoPolling = false;
		DWORD m_OldConsoleMode;

		// Reapplies the console mode from the union of all registered readers' flags; must be
		// called with the subsystem lock held whenever the reader set changes
		void f_UpdateConsoleMode()
		{
			if (!m_bIsChar)
				return;

			bool bVirtualTerminalInput = false;
			for (auto iReader = m_Readers.f_GetIterator(); iReader; ++iReader)
			{
				if (iReader->m_pParams->m_Flags & EStdInReaderFlag_VirtualTerminalInput)
					bVirtualTerminalInput = true;
			}

			DWORD NewConsoleMode = m_OldConsoleMode & ~DWORD(ENABLE_LINE_INPUT);
			if (bVirtualTerminalInput)
			{
				// Without virtual terminal input the console only forwards character input;
				// non character key records and mouse records never reach ReadConsoleW
				NewConsoleMode = (NewConsoleMode | DWORD(ENABLE_VIRTUAL_TERMINAL_INPUT)) & ~DWORD(ENABLE_ECHO_INPUT);
			}

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
				if
					(
						NLocal::g_VersionInfo.dwPlatformId == VER_PLATFORM_WIN32_NT
						&&
						(
							NLocal::g_VersionInfo.dwMajorVersion < 6
							|| (NLocal::g_VersionInfo.dwMajorVersion == 6 && NLocal::g_VersionInfo.dwMinorVersion < 3)
						)
					)
				{
					// Can't cancel read console on older versions of windows
					m_bDoPolling = true;
				}

			}
			else if (HandleType == FILE_TYPE_PIPE)
			{
				m_bIsPipe = true;
			}
			else
				DMibError("Stdin handle is not of a supported type (char or pipe)");

			f_Start();
		}

	private:

		NMib::NStr::CStr f_GetThreadName()
		{
			return "Stdin reader";
		}
		aint f_Main()
		{

			fp64 TotalReadTime = 0.0;
			fp64 TotalWaitTime = 0.0;
			while (f_GetState() != NMib::NThread::EThreadState_EventWantQuit)
			{
				NTime::CTimeMeasure Timer;
				Timer.f_Start();
				bool bReadSuccess = fp_Read();
				Timer.f_Stop();
				fp64 ReadTime = Timer.f_GetTime();
				TotalReadTime += ReadTime;

				if (m_bDoPolling)
				{
					if (m_bIsPipe)
					{
						Timer.f_Start();
						m_EventWantQuit.f_WaitTimeout(0.016); // Poll ever 16 ms as there is no way to wait for input on a synchronous pipe
						Timer.f_Stop();
						TotalWaitTime += Timer.f_GetTime();
					}
					else
					{
						HANDLE ToWaitFor[] = {m_EventWantQuit.m_pSemaphore, mp_hStdInFile};

						{
							Timer.f_Start();
							WaitForMultipleObjectsEx(2, ToWaitFor, false, INFINITE, true);
								//WaitForSingleObject(mp_hStdInFile, 2000);
							Timer.f_Stop();
						}

						TotalWaitTime += Timer.f_GetTime();
						m_EventWantQuit.f_TryWait();
					}
				}
				else if (!bReadSuccess)
					m_EventWantQuit.f_Wait();
			}

			if (m_bDoPolling)
				fp_Read();

			return 0;
		}

		HANDLE mp_hStdInFile;

		NContainer::CIOByteVector mp_StdInReadBuffer;
		NStr::CWStrIO mp_StdInStringBuffer;


		bool fp_Read()
		{
			bool bRet = true;
			if (m_bDoPolling)
			{
				if (m_bIsPipe)
				{
					if (mp_StdInReadBuffer.f_IsEmpty())
						mp_StdInReadBuffer.f_SetLen(4096);

					while (true)
					{
						DWORD nBytesRead = 0;
						DWORD nTotalBytesAvailable;
						DWORD nBytesLeftThisMessage;
						if (PeekNamedPipe(mp_hStdInFile, mp_StdInReadBuffer.f_GetArray(), 4096, &nBytesRead, &nTotalBytesAvailable, &nBytesLeftThisMessage))
						{
							if (nBytesRead)
							{
								// Read for real
								if (!ReadFile(mp_hStdInFile, mp_StdInReadBuffer.f_GetArray(), 4096, &nBytesRead, nullptr))
									bRet = false;

								if (nBytesRead)
									fp_SendToReaders(EStdInReaderOutputType_StdIn, NContainer::CIOByteVector(mp_StdInReadBuffer.f_GetArray(), nBytesRead));
							}
						}
						else
						{
							DWORD dwError = GetLastError();

							if (dwError == ERROR_BROKEN_PIPE || dwError == ERROR_NO_DATA || dwError == ERROR_HANDLE_EOF)
								fp_SendToReaders(EStdInReaderOutputType_EndOfFile, "End of file");
							else
								fp_SendToReaders(EStdInReaderOutputType_GeneralError, "PeekNamedPipe failed: {}"_f << NMib::NPlatform::fg_Win32_GetLastErrorStr(dwError));
							bRet = false;
						}
						if (!nBytesRead)
							break;
					}
				}
				else if (m_bIsChar)
				{
					auto fl_RemoveNonKeyEvents
						= [&]()
						{
							while (true)
							{
								INPUT_RECORD InputRecord;
								DWORD nReadEvents = 0;
								if (PeekConsoleInputW(mp_hStdInFile, &InputRecord, 1, &nReadEvents))
								{
									if (nReadEvents < 1)
										break;
									if (InputRecord.EventType == KEY_EVENT && InputRecord.Event.KeyEvent.uChar.UnicodeChar && InputRecord.Event.KeyEvent.bKeyDown)
										break;
									if (!ReadConsoleInputW(mp_hStdInFile, &InputRecord, 1, &nReadEvents))
										break;
								}
								else
									break;
							}
						}
					;

					NStr::CWStrIO ToSend;
					while (true)
					{
						DWORD LastAvailable;
						if (!GetNumberOfConsoleInputEvents(mp_hStdInFile, &LastAvailable))
							bRet = false;

						while (1)
						{
							fl_RemoveNonKeyEvents();
							DWORD nAvailable = 0;
							GetNumberOfConsoleInputEvents(mp_hStdInFile, &nAvailable);

							if (nAvailable == LastAvailable)
							{
								LastAvailable = nAvailable;
								break;
							}
							LastAvailable = nAvailable;
						}
						if (LastAvailable)
						{
							NStr::CWStr Temp;
							DWORD nReadEvents = 0;
							NContainer::TCVector<INPUT_RECORD> Records;
							Records.f_SetLen(LastAvailable);
							bool bCharAvailable = false;
							if (PeekConsoleInputW(mp_hStdInFile, Records.f_GetArray(), LastAvailable, &nReadEvents))
							{
								for (umint i = 0; i < nReadEvents; ++i)
								{
									if (Records[i].EventType == KEY_EVENT && Records[i].Event.KeyEvent.uChar.UnicodeChar && Records[i].Event.KeyEvent.bKeyDown)
									{
										bCharAvailable = true;
										break;
									}
								}
							}
							else
								bRet = false;
							if (!bCharAvailable)
								continue;

							DWORD nReadChars = 0;
							if (!::ReadConsoleW(mp_hStdInFile, Temp.f_GetStr(2), 1, &nReadChars, nullptr))
								bRet = false;

							if (nReadChars == 0)
								break;
							else
							{
								Temp.f_SetAt(1, 0);
								ToSend += Temp;
							}
						}
						else
							break;
					}
					if (!ToSend.f_IsEmpty())
						fp_SendToReaders(EStdInReaderOutputType_StdIn, NStr::CStrIO(ToSend));
				}
			}
			else
			{
				while (true)
				{
					if (m_bIsPipe)
					{
						if (mp_StdInReadBuffer.f_IsEmpty())
							mp_StdInReadBuffer.f_SetLen(4096);

						DWORD nBytesRead = 0;
						if (!ReadFile(mp_hStdInFile, mp_StdInReadBuffer.f_GetArray(), 4096, &nBytesRead, nullptr))
						{
							DWORD dwError = GetLastError();

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

						if (nBytesRead != 4096)
							break;
					}
					else
					{
						DWORD nReadChars = 0;
						if (!::ReadConsoleW(mp_hStdInFile, mp_StdInStringBuffer.f_GetStr(4097), 4096, &nReadChars, nullptr))
							bRet = false;
						else if (nReadChars)
						{
							mp_StdInStringBuffer.f_SetAt(nReadChars, 0);
							fp_SendToReaders(EStdInReaderOutputType_StdIn, NStr::CStrIO(mp_StdInStringBuffer));
						}

						if (nReadChars != 4096)
							break;
					}
				}
			}

			return bRet;
		}
		void fp_SendToReaders(EStdInReaderOutputType _Type, NStr::CStrIO const &_String);
		void fp_SendToReaders(NMib::NProcess::EStdInReaderOutputType _Type, NContainer::CIOByteVector const &_Buffer);
	};

	struct CSubSystem_Process_Platform_Windows_StdInReader : public CSubSystem
	{
		NThread::CMutual m_StdInReaderImpLock;
		NMib::NStorage::TCUniquePointer<CWindowsStdInReaderImplementation> m_pStdInReaderImp;

		void f_DestroyThreadSpecific() override
		{
			{
				DMibLock(m_StdInReaderImpLock);
				DMibFastCheck(m_pStdInReaderImp.f_IsEmpty()); // Should have been deleted when the last std in reader was closed
				m_pStdInReaderImp.f_Clear();
			}
		}

		~CSubSystem_Process_Platform_Windows_StdInReader()
		{
			{
				DMibLock(m_StdInReaderImpLock);
				DMibFastCheck(m_pStdInReaderImp.f_IsEmpty());
			}
		}
	};

	constinit TCSubSystem<CSubSystem_Process_Platform_Windows_StdInReader, ESubSystemDestruction_BeforeMemoryManager> g_SubSystem_Process_Platform_Windows_StdInReader = {DAggregateInit};

	CWindowsStdInReader::~CWindowsStdInReader()
	{
		if (m_Link.f_IsInList())
		{
			auto &SubSystem = *g_SubSystem_Process_Platform_Windows_StdInReader;
			NStorage::TCUniquePointer<CWindowsStdInReaderImplementation> pToDelete;
			{
				DMibLock(SubSystem.m_StdInReaderImpLock);
				NStorage::TCPointer<CWindowsStdInReaderImplementation> pImp = SubSystem.m_pStdInReaderImp.f_Get();
				m_Link.f_Unlink();
				if (pImp->m_Readers.f_IsEmpty())
					pToDelete = fg_Move(SubSystem.m_pStdInReaderImp);
				else
					pImp->f_UpdateConsoleMode();
			}
		}
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
		DMibRequire(_Type != EStdInReaderOutputType_StdIn || !m_bIsPipe);
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
			pNew = fg_Construct<CWindowsStdInReaderImplementation>((Params.m_Flags & EStdInReaderFlag_ForcePolling) != 0);
			pImp = (CWindowsStdInReaderImplementation *)pNew.f_Get();
		}

		pImp->m_Readers.f_Insert(*pReader);

		if (!SubSystem.m_pStdInReaderImp)
		{
			pImp->f_Init();
			SubSystem.m_pStdInReaderImp = fg_Move(pNew);
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

