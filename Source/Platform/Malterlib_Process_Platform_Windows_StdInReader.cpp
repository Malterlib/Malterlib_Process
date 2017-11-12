// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Process/StdIn>
#include "../Malterlib_Process_Platform.h"
#include <Windows.h>
#include <Mib/Core/PlatformSpecific/WindowsOptional>

namespace NMib
{
	namespace NProcess
	{
		namespace NPlatform
		{

			struct CWindowsStdInReader
			{
				NPtr::TCSharedPointer<NMib::NProcess::CStdInReaderParams> m_pParams;
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
						m_bDoPolling = NLocal::g_fCancelSynchronousIo == nullptr || NLocal::g_fCancelIoEx == nullptr;
					mp_hStdInFile = GetStdHandle(STD_INPUT_HANDLE);
				}
				~CWindowsStdInReaderImplementation()
				{
					if (m_bDoPolling)
						f_Stop(true);
					else
					{
						f_Stop(false);
						NLocal::g_fCancelSynchronousIo(f_GetThread());
						NLocal::g_fCancelIoEx(mp_hStdInFile, nullptr);
						while (f_GetState() >= NThread::EThreadState_Running)
						{
							NLocal::g_fCancelSynchronousIo(f_GetThread());
							NLocal::g_fCancelIoEx(mp_hStdInFile, nullptr);
							NSys::fg_Thread_SmallestSleep();
						}
						f_Stop(true);
					}
					if (m_bIsChar)
						SetConsoleMode(mp_hStdInFile, m_OldConsoleMode);
				}

				zbool m_bIsPipe;
				zbool m_bIsChar;
				zbool m_bDoPolling;
				DWORD m_OldConsoleMode;

				void f_Init()
				{
					if (!mp_hStdInFile)
						return;

					auto HandleType = GetFileType(mp_hStdInFile);
					if (HandleType == FILE_TYPE_CHAR)
					{
						GetConsoleMode(mp_hStdInFile, &m_OldConsoleMode);
						SetConsoleMode(mp_hStdInFile, m_OldConsoleMode & ~DWORD(ENABLE_LINE_INPUT));
						m_bIsChar = true;
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
					else if (HandleType = FILE_TYPE_PIPE)
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
						NTime::CTimer Timer;
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

								DWORD WaitObject;
								{
									Timer.f_Start();
									WaitObject = WaitForMultipleObjectsEx(2, ToWaitFor, false, INFINITE, true);
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

				NContainer::CSecureByteVector mp_StdInReadBuffer;
				NStr::CWStrSecure mp_StdInStringBuffer;


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
											fp_SendToReaders(EStdInReaderOutputType_StdIn, NContainer::CSecureByteVector(mp_StdInReadBuffer.f_GetArray(), nBytesRead));
									}
								}
								else
									bRet = false;
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
										DWORD nReadChars = 0;
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

							NStr::CWStrSecure ToSend;
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
										for (mint i = 0; i < nReadEvents; ++i)
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
								fp_SendToReaders(EStdInReaderOutputType_StdIn, NStr::CStrSecure(ToSend));
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
									bRet = false;

								if (nBytesRead)
									fp_SendToReaders(EStdInReaderOutputType_StdIn, NContainer::CSecureByteVector(mp_StdInReadBuffer.f_GetArray(), nBytesRead));
								else if (bRet)
								{
									// Does nBytesRead == 0 mean we have found EOF?
									fp_SendToReaders(EStdInReaderOutputType_EndOfFile, "End of file");
									bRet = false;
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
									fp_SendToReaders(EStdInReaderOutputType_StdIn, NStr::CStrSecure(mp_StdInStringBuffer));
								}

								if (nReadChars != 4096)
									break;
							}
						}
					}

					return bRet;
				}
				void fp_SendToReaders(EStdInReaderOutputType _Type, NStr::CStrSecure const &_String);
				void fp_SendToReaders(NMib::NProcess::EStdInReaderOutputType _Type, NContainer::CSecureByteVector const &_Buffer);
			};

			struct CSubSystem_Process_Platform_Windows_StdInReader : public CSubSystem
			{
				NThread::CMutual m_StdInReaderImpLock;
				NMib::NPtr::TCUniquePointer<CWindowsStdInReaderImplementation> m_pStdInReaderImp;
				
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
			
			TCSubSystem<CSubSystem_Process_Platform_Windows_StdInReader, ESubSystemDestruction_BeforeMemoryManager> g_SubSystem_Process_Platform_Windows_StdInReader = {DAggregateInit};

			CWindowsStdInReader::~CWindowsStdInReader()
			{
				if (m_Link.f_IsInList())
				{
					auto &SubSystem = *g_SubSystem_Process_Platform_Windows_StdInReader;
					NPtr::TCUniquePointer<CWindowsStdInReaderImplementation> pToDelete;
					{
						DMibLock(SubSystem.m_StdInReaderImpLock);
						NPtr::TCPointer<CWindowsStdInReaderImplementation> pImp = SubSystem.m_pStdInReaderImp.f_Get();
						m_Link.f_Unlink();
						if (pImp->m_Readers.f_IsEmpty())
							pToDelete = fg_Move(SubSystem.m_pStdInReaderImp);
					}
				}
			}

			void CWindowsStdInReaderImplementation::fp_SendToReaders(EStdInReaderOutputType _Type, NContainer::CSecureByteVector const &_Buffer)
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
									[_Type, String = NStr::CStrSecure(_Buffer.f_GetArray(), _Buffer.f_GetLen()), pParams]()
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
							pParams->m_fOnReceiveInput(_Type, NStr::CStrSecure(_Buffer.f_GetArray(), _Buffer.f_GetLen()));
						else
							pParams->m_fOnReceiveBinaryInput(_Type, _Buffer, {});
					}
				}
			}

			void CWindowsStdInReaderImplementation::fp_SendToReaders(EStdInReaderOutputType _Type, NStr::CStrSecure const &_String)
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
	}
}

void *NMib::NProcess::NPlatform::fg_Process_StdInReader_Open(NMib::NProcess::CStdInReaderParams &&_Params)
{
	NPtr::TCUniquePointer<CWindowsStdInReaderImplementation> pNew; // First because we want it to be destroyed after pReader in case of exception in f_Init
	NPtr::TCUniquePointer<CWindowsStdInReader> pReader = fg_Construct(fg_Move(_Params));
	
	auto &Params = *pReader->m_pParams;

	if (Params.m_fOnReceiveInput.f_IsEmpty() && Params.m_fOnReceiveBinaryInput.f_IsEmpty())
		DMibError("No on receive input function specified for stdin reader");

	if (Params.m_fOnReceiveInput.f_IsEmpty() == Params.m_fOnReceiveBinaryInput.f_IsEmpty())
		DMibError("Both string and binary on receive input function specified for stdin reader");

	auto &SubSystem = *g_SubSystem_Process_Platform_Windows_StdInReader;

	{
		DMibLock(SubSystem.m_StdInReaderImpLock);

		NPtr::TCPointer<CWindowsStdInReaderImplementation> pImp = SubSystem.m_pStdInReaderImp.f_Get();

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

		bool bInit = false;
		if (!pImp)
		{
			pNew = fg_Construct<CWindowsStdInReaderImplementation>((Params.m_Flags & EStdInReaderFlag_ForcePolling) != 0);
			pImp = (CWindowsStdInReaderImplementation *)pNew.f_Get();
			bInit = true;
		}

		pImp->m_Readers.f_Insert(*pReader);

		if (!SubSystem.m_pStdInReaderImp)
		{
			pImp->f_Init();
			SubSystem.m_pStdInReaderImp = fg_Move(pNew);
		}
	}

	return pReader.f_Detach();
}

void NMib::NProcess::NPlatform::fg_Process_StdInReader_Close(void *_pStdInReader)
{
	NPtr::TCUniquePointer<CWindowsStdInReader> pReader = fg_Explicit((CWindowsStdInReader *)_pStdInReader);
}

