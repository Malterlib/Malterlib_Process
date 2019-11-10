// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Process/StdIn>
#include <Mib/Core/PlatformSpecific/PosixErrNo>
#include <Mib/Core/PlatformSpecific/PosixFork>
#ifdef DPlatformFamily_Linux
#	include <Mib/Core/PlatformSpecific/LinuxOptional>
#endif

#include "../Malterlib_Process_Platform.h"

#include <termios.h>
#include <poll.h>
#include <fcntl.h>
#include <errno.h>

namespace NMib::NProcess::NPlatform
{
	struct CPOSIXStdInReader
	{
		NMib::NStorage::TCSharedPointer<NMib::NProcess::CStdInReaderParams> m_pParams;
		DMibListLinkDS_Link(CPOSIXStdInReader, m_Link);

		CPOSIXStdInReader(NMib::NProcess::CStdInReaderParams &&_Params)
			: m_pParams(fg_Construct(fg_Move(_Params)))
		{
		}
		CPOSIXStdInReader()
		{
		}
		~CPOSIXStdInReader();
	};

	struct CPOSIXStdInReaderImplementation : NMib::NThread::CThread
	{
		DMibListLinkDS_List(CPOSIXStdInReader, m_Link) m_Readers;


		CPOSIXStdInReaderImplementation(bool _bForcePolling)
			: mp_WakeupPipeRead(-1)
			, mp_WakeupPipeWrite(-1)
			, m_OldFlags(-1)
		{
		}
		~CPOSIXStdInReaderImplementation()
		{
			f_Stop(false);

			if (mp_WakeupPipeWrite != -1)
			{
				ch8 Temp = 0;
				write(mp_WakeupPipeWrite, &Temp, sizeof(Temp));
			}

			f_Stop(true);

			fp_DestroyPipe(mp_WakeupPipeRead);
			fp_DestroyPipe(mp_WakeupPipeWrite);

			fcntl(0, F_SETFL, m_OldFlags);

			if (m_bOldSettingsSet)
				tcsetattr(0, TCSANOW, &m_OldSettings);
		}

		void f_Init()
		{
			m_OldFlags = fcntl(0, F_GETFL);

			if (m_OldFlags == -1)
				return;

			if (fcntl(0, F_SETFL, m_OldFlags | O_NONBLOCK) == -1)
			{
				m_OldFlags = -1;
				return;
			}

			if (tcgetattr (0, &m_OldSettings) >= 0)
			{
				auto NewSettings = m_OldSettings;
				// Emulate the behaviour of cfmakeraw() but without breaking NL
				NewSettings.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
//				NewSettings.c_oflag &= ~OPOST;
				NewSettings.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
				NewSettings.c_cflag &= ~(CSIZE | PARENB);
				NewSettings.c_cflag |= CS8;
				tcsetattr(0, TCSANOW, &NewSettings);
				m_bOldSettingsSet = true;
			}

			fs_CreatePipe(mp_WakeupPipeRead, mp_WakeupPipeWrite);
			fcntl(mp_WakeupPipeRead, F_SETFL, O_NONBLOCK);

			f_Start();
		}

	private:

		struct termios m_OldSettings;
		bool m_bOldSettingsSet = false;
		int m_OldFlags;
		int mp_WakeupPipeRead;
		int mp_WakeupPipeWrite;

		void fp_DestroyPipe(int &_Handle)
		{
			if (_Handle != -1)
			{
				close(_Handle);
				_Handle = -1;
			}
		}
		static bool fs_CreatePipe(int &_Read, int &_Write)
		{
			DMibFastCheck(_Read == -1);
			DMibFastCheck(_Write == -1);
			int Pipes[2];
#ifdef DPlatformFamily_Linux
			if (NLocal::g_f_pipe2)
			{
				if (NLocal::g_f_pipe2(Pipes, O_CLOEXEC))
					return false;
			}
			else
#elif defined(DPlatformFamily_Emscripten)
			if (pipe2(Pipes, O_CLOEXEC))
				return false;
			else
#endif
			{
				// We need to make sure that we protect the pipes against being included in other processes
				DMibLock(NMib::NPlatform::fg_ForkLock());
				if (pipe(Pipes))
					return false;

				fcntl(Pipes[0], F_SETFD, fcntl(Pipes[0], F_GETFD) | FD_CLOEXEC);
				fcntl(Pipes[1], F_SETFD, fcntl(Pipes[1], F_GETFD) | FD_CLOEXEC);
			}

#ifdef F_SETNOSIGPIPE
			fcntl(Pipes[0], F_SETNOSIGPIPE, 1);
			fcntl(Pipes[1], F_SETNOSIGPIPE, 1);
#endif

			_Read = Pipes[0];
			_Write = Pipes[1];
			return true;
		}

		NMib::NStr::CStr f_GetThreadName()
		{
			return "Stdin reader";
		}
		aint f_Main()
		{
			while (f_GetState() != NMib::NThread::EThreadState_EventWantQuit)
			{
				if (!fp_Read())
					break;

				pollfd ToPoll[2] = {0};
				int nPoll = 0;
				ToPoll[nPoll].fd = mp_WakeupPipeRead;
				ToPoll[nPoll].events = POLLRDNORM;
				ToPoll[nPoll].revents = 0;
				++nPoll;

				ToPoll[nPoll].fd = 0; // Std-in
				ToPoll[nPoll].events = POLLRDNORM | POLLIN;
				ToPoll[nPoll].revents = 0;
				++nPoll;

				int PollReturn = poll(ToPoll, nPoll, -1);

				if (PollReturn == -1)
				{
					int ErrNo = errno;
					if (ErrNo != EINTR)
					{
						fp_SendToReaders(NMib::NProcess::EStdInReaderOutputType_GeneralError, NMib::NPlatform::fg_FormatErrno("poll (stdin reader)", ErrNo) + "\n");
						break;
					}
				}
			}

			fp_Read();

			return 0;
		}

		NContainer::CSecureByteVector mp_StdInReadBuffer;


		bool fp_Read()
		{
			bool bRet = true;

			if (mp_StdInReadBuffer.f_IsEmpty())
				mp_StdInReadBuffer.f_SetLen(4096);

			auto ReadBytes = read(0, mp_StdInReadBuffer.f_GetArray(), 4096);

			if (ReadBytes > 0)
				fp_SendToReaders(NMib::NProcess::EStdInReaderOutputType_StdIn, NContainer::CSecureByteVector(mp_StdInReadBuffer.f_GetArray(), ReadBytes));
			else if (ReadBytes < 0)
			{
				int ErrNo = errno;

				if (ErrNo == EAGAIN || ErrNo == EWOULDBLOCK)
				{
					// Normal
				}
				else
				{
					fp_SendToReaders(NMib::NProcess::EStdInReaderOutputType_GeneralError, NMib::NPlatform::fg_FormatErrno("read (stdin reader)", ErrNo) + "\n");
					bRet = false;
				}
			}
			else
			{
				fp_SendToReaders(NMib::NProcess::EStdInReaderOutputType_EndOfFile, "End of file");
				bRet = false;
			}

			return bRet;
		}

		void fp_SendToReaders(NMib::NProcess::EStdInReaderOutputType _Type, NContainer::CSecureByteVector const &_Buffer);
		void fp_SendToReaders(NMib::NProcess::EStdInReaderOutputType _Type, NStr::CStrSecure const &_String);
	};

	struct CSubSystem_Process_Platform_POSIX_StdInReader : public CSubSystem
	{
		NThread::CMutual m_StdInReaderImpLock;
		NMib::NStorage::TCUniquePointer<CPOSIXStdInReaderImplementation> m_pStdInReaderImp;

		void f_DestroyThreadSpecific() override
		{
			{
				DMibLock(m_StdInReaderImpLock);
				DMibFastCheck(m_pStdInReaderImp.f_IsEmpty()); // Should have been deleted when the last std in reader was closed
				m_pStdInReaderImp.f_Clear();
			}
		}

		~CSubSystem_Process_Platform_POSIX_StdInReader()
		{
			{
				DMibLock(m_StdInReaderImpLock);
				DMibFastCheck(m_pStdInReaderImp.f_IsEmpty());
			}
		}
	};

	constinit TCSubSystem<CSubSystem_Process_Platform_POSIX_StdInReader, ESubSystemDestruction_BeforeMemoryManager> g_SubSystem_Process_Platform_POSIX_StdInReader = {DAggregateInit};

	CPOSIXStdInReader::~CPOSIXStdInReader()
	{
		if (m_Link.f_IsInList())
		{
			auto &SubSystem = *g_SubSystem_Process_Platform_POSIX_StdInReader;
			NMib::NStorage::TCUniquePointer<CPOSIXStdInReaderImplementation> pToDelete;
			{
				DMibLock(SubSystem.m_StdInReaderImpLock);
				NMib::NStorage::TCPointer<CPOSIXStdInReaderImplementation> pImp = SubSystem.m_pStdInReaderImp.f_Get();
				m_Link.f_Unlink();
				if (pImp->m_Readers.f_IsEmpty())
					pToDelete = fg_Move(SubSystem.m_pStdInReaderImp);
			}
		}
	}
	void CPOSIXStdInReaderImplementation::fp_SendToReaders(NMib::NProcess::EStdInReaderOutputType _Type, NContainer::CSecureByteVector const &_Buffer)
	{
		DMibRequire(_Type == EStdInReaderOutputType_StdIn);

		auto &SubSystem = *g_SubSystem_Process_Platform_POSIX_StdInReader;
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

	void CPOSIXStdInReaderImplementation::fp_SendToReaders(NMib::NProcess::EStdInReaderOutputType _Type, NStr::CStrSecure const &_String)
	{
		DMibRequire(_Type != EStdInReaderOutputType_StdIn);

		auto &SubSystem = *g_SubSystem_Process_Platform_POSIX_StdInReader;
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
	NStorage::TCUniquePointer<CPOSIXStdInReaderImplementation> pNew; // First because we want it to be destroyed after pReader in case of exception in f_Init
	NStorage::TCUniquePointer<CPOSIXStdInReader> pReader = fg_Construct(fg_Move(_Params));

	auto &Params = *pReader->m_pParams;

	if (Params.m_fOnReceiveInput.f_IsEmpty() && Params.m_fOnReceiveBinaryInput.f_IsEmpty())
		DMibError("No on receive input function specified for stdin reader");

	if (Params.m_fOnReceiveInput.f_IsEmpty() == Params.m_fOnReceiveBinaryInput.f_IsEmpty())
		DMibError("Both string and binary on receive input function specified for stdin reader");

	auto &SubSystem = *g_SubSystem_Process_Platform_POSIX_StdInReader;

	{
		DMibLock(SubSystem.m_StdInReaderImpLock);

		NStorage::TCPointer<CPOSIXStdInReaderImplementation> pImp = SubSystem.m_pStdInReaderImp.f_Get();

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
			pNew = fg_Construct<CPOSIXStdInReaderImplementation>((Params.m_Flags & EStdInReaderFlag_ForcePolling) != 0);
			pImp = (CPOSIXStdInReaderImplementation *)pNew.f_Get();
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
	NStorage::TCUniquePointer<CPOSIXStdInReader> pReader = fg_Explicit((CPOSIXStdInReader *)_pStdInReader);
}
