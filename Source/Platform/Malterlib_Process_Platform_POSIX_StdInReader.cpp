// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Process/StdIn>
#include <Mib/Core/PlatformSpecific/PosixErrNo>
#include <Mib/Core/PlatformSpecific/PosixFork>
#ifdef DPlatformFamily_Linux
#	include <Mib/Core/PlatformSpecific/LinuxOptional>
#endif

#include "../Malterlib_Process_Platform.h"

#include <termios.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>

namespace NMib::NProcess::NPlatform
{
	struct CPOSIXStdInReader
	{
		CPOSIXStdInReader(NMib::NProcess::CStdInReaderParams &&_Params)
			: m_pParams(fg_Construct(fg_Move(_Params)))
		{
		}
		CPOSIXStdInReader()
		{
		}
		~CPOSIXStdInReader();

		NMib::NStorage::TCSharedPointer<NMib::NProcess::CStdInReaderParams> m_pParams;
		DMibListLinkDS_Link(CPOSIXStdInReader, m_Link);
	};

	// Bytes per read. A pipe never returns more than it holds, 64 KiB on Linux and macOS, and every
	// chunk costs a system call and a delivery to the readers, which is an actor hop, so anything
	// smaller than the pipe bounds a piped bulk input by that overhead rather than by memory copy
	constexpr umint gc_StdInReadChunkBytes = 64 * 1024;

	// Standard input as a descriptor on an io loop: the loop reports it readable, this reads until
	// it would block and asks to be told again. No thread of its own; a caller that opens the first
	// reader inside a CIoLoopCreateScope gets its input on that loop's thread, everyone else on the
	// process wide poller, the same way the signal subsystem places its dispatch. The loop is
	// chosen when the first reader opens; the registration itself is issued by the subsystem, at
	// once or once a previous implementation's removal has been acknowledged, since standard
	// input can be registered only once at a time.
	//
	// A regular file redirected onto standard input is always readable and epoll refuses to watch
	// one, so it is not registered at all: a self pipe stands in for it, and every pass reads one
	// chunk and rewrites the pipe to schedule the next, which paces the file through the loop
	// instead of delivering it in one uninterruptible burst
	struct CPOSIXStdInReaderImplementation
	{
		CPOSIXStdInReaderImplementation()
		{
		}
		~CPOSIXStdInReaderImplementation()
		{
			DMibFastCheck(!mp_pRegistration);

			fp_DestroyPipe(mp_PacePipe[0]);
			fp_DestroyPipe(mp_PacePipe[1]);

			if (mp_OldFlags != -1)
				fcntl(0, F_SETFL, mp_OldFlags);

			if (mp_bOldSettingsSet)
				tcsetattr(0, TCSANOW, &mp_OldSettings);
		}

		void f_Init()
		{
			mp_OldFlags = fcntl(0, F_GETFL);

			if (mp_OldFlags != -1 && fcntl(0, F_SETFL, mp_OldFlags | O_NONBLOCK) == -1)
				mp_OldFlags = -1;

			if (tcgetattr(0, &mp_OldSettings) >= 0)
			{
				auto NewSettings = mp_OldSettings;
				// Emulate the behaviour of cfmakeraw() but without breaking NL
				NewSettings.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
//				NewSettings.c_oflag &= ~OPOST;
				NewSettings.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
				NewSettings.c_cflag &= ~(CSIZE | PARENB);
				NewSettings.c_cflag |= CS8;
				tcsetattr(0, TCSANOW, &NewSettings);
				mp_bOldSettingsSet = true;
			}

			mp_pLoop = NSys::fg_GetThreadIoLoop();
			if (!mp_pLoop)
				mp_pLoop = NSys::fg_GetSharedIoLoop();
			if (!mp_pLoop)
				DMibError("No io loop is available to read standard input on");

			struct stat Stat;
			if (fstat(0, &Stat) == 0 && S_ISREG(Stat.st_mode))
			{
				if (!fs_CreatePipe(mp_PacePipe[0], mp_PacePipe[1]))
					DMibError("Failed to create the pacing pipe for reading standard input from a file");

				mp_bPaced = true;
				fp_SchedulePacedRead();
			}
		}

		// Puts the descriptor on the loop; callable from any thread, so the subsystem can issue it
		// from a previous removal's continuation
		void f_Register()
		{
			DMibFastCheck(!mp_pRegistration);

			mp_pRegistration = mp_pLoop->f_Register
				(
					mp_bPaced ? mp_PacePipe[0] : 0
					, this
					, NSys::EIoLoopEvent::mc_Read
					, &fs_OnReadable
					, false
				)
			;
		}

		// Hands the registration to whoever removes it, null when none was issued yet
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

		DMibListLinkDS_List(CPOSIXStdInReader, m_Link) m_Readers;

	private:
		static void fp_DestroyPipe(int &_Handle)
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
				if (NLocal::g_f_pipe2(Pipes, O_CLOEXEC | O_NONBLOCK))
					return false;
			}
			else
#elif defined(DPlatformFamily_Emscripten)
			if (pipe2(Pipes, O_CLOEXEC | O_NONBLOCK))
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
				fcntl(Pipes[0], F_SETFL, fcntl(Pipes[0], F_GETFL) | O_NONBLOCK);
				fcntl(Pipes[1], F_SETFL, fcntl(Pipes[1], F_GETFL) | O_NONBLOCK);
			}

#ifdef F_SETNOSIGPIPE
			fcntl(Pipes[0], F_SETNOSIGPIPE, 1);
			fcntl(Pipes[1], F_SETNOSIGPIPE, 1);
#endif

			_Read = Pipes[0];
			_Write = Pipes[1];
			return true;
		}

		// A byte in the pace pipe is the next pass; one already queued guarantees it
		void fp_SchedulePacedRead()
		{
			ch8 Byte = 'p';
			(void)!write(mp_PacePipe[1], &Byte, 1);
		}

		static void fs_OnReadable(void *_pToken, NSys::EIoLoopEvent _Events, int _Error)
		{
			static_cast<CPOSIXStdInReaderImplementation *>(_pToken)->fp_OnReadable(_Events, _Error);
		}

		void fp_OnReadable(NSys::EIoLoopEvent _Events, int _Error)
		{
			if (mp_bFinished)
				return;

			if (fg_IsSet(_Events, NSys::EIoLoopEvent::mc_Error))
			{
				// A loop that could not watch the descriptor says so once; nothing follows
				mp_bFinished = true;
				fp_SendToReaders(EStdInReaderOutputType_GeneralError, NMib::NPlatform::fg_FormatErrno("io loop (stdin reader)", _Error ? _Error : EIO) + "\n");
				return;
			}

			if (mp_bPaced)
			{
				ch8 Bytes[64];
				while (read(mp_PacePipe[0], Bytes, sizeof(Bytes)) > 0)
					; // Drained in whole, which is the would-block observation the request rests on

				mp_pLoop->f_RequestReadiness(mp_pRegistration, NSys::EIoLoopEvent::mc_Read);

				// One chunk per pass, then the next pass is scheduled: the loop's other work gets
				// its turn between the chunks of a large file. The byte lands after the request,
				// so every backend sees it as the transition it was asked about
				if (fp_Read(true))
					fp_SchedulePacedRead();
				else
					mp_bFinished = true;

				return;
			}

			if (fp_Read(false))
				mp_pLoop->f_RequestReadiness(mp_pRegistration, NSys::EIoLoopEvent::mc_Read);
			else
				mp_bFinished = true;
		}

		// Reads what is available: one chunk when paced, otherwise until the descriptor would
		// block, which is the observation the readiness request that follows rests on. False at
		// end of file or on an error, both reported to the readers
		bool fp_Read(bool _bSingleChunk)
		{
			if (mp_StdInReadBuffer.f_IsEmpty())
				mp_StdInReadBuffer.f_SetLen(gc_StdInReadChunkBytes);

			while (true)
			{
				auto ReadBytes = read(0, mp_StdInReadBuffer.f_GetArray(), gc_StdInReadChunkBytes);

				if (ReadBytes > 0)
				{
					fp_SendToReaders(EStdInReaderOutputType_StdIn, NContainer::CIOByteVector(mp_StdInReadBuffer.f_GetArray(), ReadBytes));

					if (_bSingleChunk)
						return true;

					continue;
				}

				if (ReadBytes < 0)
				{
					int ErrNo = errno;

					if (ErrNo == EINTR)
						continue;

					if (ErrNo == EAGAIN || ErrNo == EWOULDBLOCK)
						return true;

					fp_SendToReaders(EStdInReaderOutputType_GeneralError, NMib::NPlatform::fg_FormatErrno("read (stdin reader)", ErrNo) + "\n");
					return false;
				}

				fp_SendToReaders(EStdInReaderOutputType_EndOfFile, "End of file");
				return false;
			}
		}

		void fp_SendToReaders(NMib::NProcess::EStdInReaderOutputType _Type, NContainer::CIOByteVector const &_Buffer);
		void fp_SendToReaders(NMib::NProcess::EStdInReaderOutputType _Type, NStr::CStrIO const &_String);

		struct termios mp_OldSettings;
		NSys::ICIoLoop *mp_pLoop = nullptr;
		NSys::CIoLoopRegistration *mp_pRegistration = nullptr;
		NContainer::CIOByteVector mp_StdInReadBuffer;
		int mp_PacePipe[2] = {-1, -1};
		int mp_OldFlags = -1;
		bool mp_bOldSettingsSet = false;
		bool mp_bPaced = false;

		// End of file or an error has been reported; nothing further is read or asked for
		bool mp_bFinished = false;
	};

	struct CSubSystem_Process_Platform_POSIX_StdInReader : public CSubSystem
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

		~CSubSystem_Process_Platform_POSIX_StdInReader()
		{
			{
				DMibLock(m_StdInReaderImpLock);
				DMibFastCheck(m_pStdInReaderImp.f_IsEmpty());
			}

			m_bWasDestroyed.f_Store(true);
		}

		NThread::CMutual m_StdInReaderImpLock;
		NMib::NStorage::TCUniquePointer<CPOSIXStdInReaderImplementation> m_pStdInReaderImp;

		// Implementations taken off the loop and not yet acknowledged. Standard input can only be
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

	constinit TCSubSystem<CSubSystem_Process_Platform_POSIX_StdInReader, ESubSystemDestruction_BeforeMemoryManager> g_SubSystem_Process_Platform_POSIX_StdInReader = {DAggregateInit};

	CPOSIXStdInReader::~CPOSIXStdInReader()
	{
		if (!m_Link.f_IsInList())
			return;

		auto &SubSystem = *g_SubSystem_Process_Platform_POSIX_StdInReader;
		NMib::NStorage::TCUniquePointer<CPOSIXStdInReaderImplementation> pToClose;
		{
			DMibLock(SubSystem.m_StdInReaderImpLock);
			NMib::NStorage::TCPointer<CPOSIXStdInReaderImplementation> pImp = SubSystem.m_pStdInReaderImp.f_Get();
			m_Link.f_Unlink();
			if (pImp->m_Readers.f_IsEmpty())
			{
				pToClose = fg_Move(SubSystem.m_pStdInReaderImp);
				++SubSystem.m_nClosing;

				// Never registered: it was itself waiting for a removal, and stops waiting
				SubSystem.m_bRegistrationPending = false;
			}
		}

		if (!pToClose)
			return;

		// Outside the lock, since the loop's callback takes it to deliver. The removal never
		// blocks: the implementation is destroyed, and the terminal restored, once the loop
		// reports that no callback can be in flight, on the loop's own thread
		auto *pLoop = pToClose->f_GetLoop();
		auto *pRegistration = pToClose->f_TakeRegistration();
		auto fOnClosed = [pToClose = fg_Move(pToClose)]() mutable
			{
				pToClose.f_Clear();
				g_SubSystem_Process_Platform_POSIX_StdInReader->f_OnClosed();
			}
		;

		if (pRegistration)
			pLoop->f_DeregisterAsync(pRegistration, fg_Move(fOnClosed));
		else
			fOnClosed();
	}

	void CPOSIXStdInReaderImplementation::fp_SendToReaders(NMib::NProcess::EStdInReaderOutputType _Type, NContainer::CIOByteVector const &_Buffer)
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

	void CPOSIXStdInReaderImplementation::fp_SendToReaders(NMib::NProcess::EStdInReaderOutputType _Type, NStr::CStrIO const &_String)
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
		DMibLock(NMib::NPlatform::fg_ForkLock()); // Need to take fork lock first to prevent cycle
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

			if (!pImp)
			{
				pNew = fg_Construct<CPOSIXStdInReaderImplementation>();
				pImp = (CPOSIXStdInReaderImplementation *)pNew.f_Get();
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
		}
	}

	return pReader.f_Detach();
}

void NMib::NProcess::NPlatform::fg_Process_StdInReader_Close(void *_pStdInReader)
{
	NStorage::TCUniquePointer<CPOSIXStdInReader> pReader = fg_Explicit((CPOSIXStdInReader *)_pStdInReader);
}

auto NMib::NProcess::NPlatform::fg_Process_StdInReader_RegisterScreenChange
	(
		NFunction::TCFunction<void (NSys::CConsoleProperties const &_ConsoleProperties)> &&_fOnScreenChange
	)
	-> NMib::COnScopeExitShared
{
	// The terminal signals a resize with SIGWINCH here, which NSys::fg_System_RegisterForSignal
	// delivers; the input stream carries nothing about it
	DMibError("Screen changes are not observed through standard input on this platform");
}
