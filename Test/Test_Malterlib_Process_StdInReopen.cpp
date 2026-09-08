// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Test/Test>
#include <Mib/Process/StdIn>
#include <Mib/Process/ProcessLaunch>
#include <Mib/Concurrency/ConcurrencyManager>

#ifdef DPlatformFamily_Windows
#	include <Windows.h>
#else
#	include <fcntl.h>
#	include <stdlib.h>
#	include <termios.h>
#	include <unistd.h>
#endif

namespace
{
	using namespace NMib;

	struct CDeferredStdInLoop : NSys::ICIoLoop
	{
		void f_WaitAndDispatch() override
		{
		}
		void f_WaitAndDispatchTimeout(pfp64) override
		{
		}
		bool f_PollAndDispatch() override
		{
			return false;
		}
		void f_Wake() override
		{
		}
		void f_SetOwnerThreadToCurrent() override
		{
		}
		void f_Deregister(NSys::CIoLoopRegistration *) override
		{
		}

		auto f_Register
			(
				NSys::CIoLoopHandle, void *, NSys::EIoLoopEvent, NSys::FIoLoopReadinessCallback, bool, NSys::CIoLoopRegisterOptions const &
			)
			-> NSys::CIoLoopRegistration * override
		{
			++m_nRegistrations;
			return reinterpret_cast<NSys::CIoLoopRegistration *>(this);
		}

		void f_DeregisterAsync(NSys::CIoLoopRegistration *, NFunction::TCFunctionMovable<void ()> &&_fOnClosed) override
		{
			DMibFastCheck(!m_fOnClosed);
			m_fOnClosed = fg_Move(_fOnClosed);
		}

		void f_CompleteRemovals()
		{
			while (m_fOnClosed)
			{
				auto fOnClosed = fg_Move(m_fOnClosed);
				fOnClosed();
			}
		}

		umint m_nRegistrations = 0;
		NFunction::TCFunctionMovable<void ()> m_fOnClosed;
	};

	struct CStdInReopen_Tests : NTest::CTest
	{
		void f_DoTests()
		{
#ifndef DPlatformFamily_Windows
			DMibTestSuite("NullInputEOF")
			{
				if (!(NTest::fg_TestReportFlags() & NTest::ETestReportFlag_ProcessRecursive))
				{
					NStr::CStr Output;
					NStr::CStr Error;
					uint32 ExitCode = 1;
					bool bLaunched = NProcess::CProcessLaunch::fs_LaunchBlock
						(
							NFile::CFile::fs_GetProgramPath()
							, {"--test", NTest::fg_TestGetCurrentPath(), "--process-recursive"}
							, Output, Error, ExitCode
						)
					;
					DMibExpectTrue(bLaunched);
					DMibExpect(ExitCode, ==, 0);
					if (ExitCode)
						DMibConErrOut("{}{}", Output, Error);

					return;
				}

				int SavedInput = dup(0);
				DMibAssert(SavedInput, >=, 0);
				auto RestoreInput = g_OnScopeExit / [&]
					{
						dup2(SavedInput, 0);
						close(SavedInput);
					}
				;
				int Null = open("/dev/null", O_RDONLY);
				DMibAssert(Null, >=, 0);
				int RedirectResult = dup2(Null, 0);
				close(Null);
				DMibAssert(RedirectResult, ==, 0);

				for (bool bBinary : {false, true})
				{
					DMibTestPath(bBinary ? "Binary" : "Text");
					NThread::CEvent Completed;
					NAtomic::TCAtomic<umint> nEOF = 0;
					NAtomic::TCAtomic<umint> nErrors = 0;
					NAtomic::TCAtomic<umint> nData = 0;
					auto fOnInput = [&](NProcess::EStdInReaderOutputType _Type)
						{
							if (_Type == NProcess::EStdInReaderOutputType_EndOfFile)
								nEOF.f_FetchAdd(1);
							else if (_Type == NProcess::EStdInReaderOutputType_GeneralError)
								nErrors.f_FetchAdd(1);
							else
								nData.f_FetchAdd(1);
							Completed.f_SetSignaled();
						}
					;
					{
						auto Params = bBinary
							? NProcess::CStdInReaderParams::fs_CreateBinary
								([&](NProcess::EStdInReaderOutputType _Type, NContainer::CIOByteVector const &, NStr::CStr const &) { fOnInput(_Type); })
							: NProcess::CStdInReaderParams::fs_Create
								([&](NProcess::EStdInReaderOutputType _Type, NStr::CStrIO const &) { fOnInput(_Type); })
						;
						NProcess::CStdInReader Reader(fg_Move(Params));
						DMibExpectFalse(Completed.f_WaitTimeout(5.0));
					}

					DMibExpect(nEOF.f_Load(), ==, umint(1));
					DMibExpect(nErrors.f_Load(), ==, umint(0));
					DMibExpect(nData.f_Load(), ==, umint(0));
				}
			};
#endif

			DMibTestSuite("DeferredModeRestoration")
			{
				if (!(NTest::fg_TestReportFlags() & NTest::ETestReportFlag_ProcessRecursive))
				{
					NStr::CStr Output;
					NStr::CStr Error;
					uint32 ExitCode = 1;
					bool bLaunched = NProcess::CProcessLaunch::fs_LaunchBlock
						(
							NFile::CFile::fs_GetProgramPath()
							, {"--test", NTest::fg_TestGetCurrentPath(), "--process-recursive"}
							, Output, Error, ExitCode
						)
					;
					DMibExpectTrue(bLaunched);
					DMibExpect(ExitCode, ==, 0);
					if (ExitCode)
						DMibConErrOut("{}{}", Output, Error);

					return;
				}

#ifdef DPlatformFamily_Windows
				HANDLE SavedInput = GetStdHandle(STD_INPUT_HANDLE);
				HANDLE SavedOutput = GetStdHandle(STD_OUTPUT_HANDLE);
				HANDLE SavedError = GetStdHandle(STD_ERROR_HANDLE);
				HANDLE Input = SavedInput;
				DWORD SavedMode = 0;
				bool bAllocatedConsole = false;
				bool bOpenedInput = false;
				auto RestoreInput = g_OnScopeExit / [&]
					{
						SetConsoleMode(Input, SavedMode);
						if (bOpenedInput)
							CloseHandle(Input);
						if (bAllocatedConsole)
							FreeConsole();
						SetStdHandle(STD_INPUT_HANDLE, SavedInput);
					}
				;
				if (!GetConsoleMode(Input, &SavedMode))
				{
					FreeConsole();
					DMibAssertTrue(AllocConsole() != 0);
					bAllocatedConsole = true;
					ShowWindow(GetConsoleWindow(), SW_HIDE);
					Input = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
					DMibAssertTrue(Input != INVALID_HANDLE_VALUE);
					bOpenedInput = true;
					SetStdHandle(STD_INPUT_HANDLE, Input);
					SetStdHandle(STD_OUTPUT_HANDLE, SavedOutput);
					SetStdHandle(STD_ERROR_HANDLE, SavedError);
					DMibAssertTrue(GetConsoleMode(Input, &SavedMode) != 0);
				}

				DWORD OriginalMode = (SavedMode | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT) & ~DWORD(ENABLE_MOUSE_INPUT);
				DMibAssertTrue(SetConsoleMode(Input, OriginalMode) != 0);
#else
				int SavedInput = dup(0);
				DMibAssert(SavedInput, >=, 0);
				int Master = -1;
				int Slave = -1;
				auto RestoreInput = g_OnScopeExit / [&]
					{
						dup2(SavedInput, 0);
						close(SavedInput);
						if (Slave >= 0)
							close(Slave);
						if (Master >= 0)
							close(Master);
					}
				;
				Master = posix_openpt(O_RDWR | O_NOCTTY);
				DMibAssert(Master, >=, 0);
				DMibAssert(grantpt(Master), ==, 0);
				DMibAssert(unlockpt(Master), ==, 0);
				Slave = open(ptsname(Master), O_RDWR | O_NOCTTY);
				DMibAssert(Slave, >=, 0);
				DMibAssert(dup2(Slave, 0), ==, 0);
				int OriginalFlags = fcntl(0, F_GETFL);
				termios OriginalMode{};
				DMibAssert(tcgetattr(0, &OriginalMode), ==, 0);
				OriginalMode.c_lflag |= ICANON | ECHO;
				DMibAssert(tcsetattr(0, TCSANOW, &OriginalMode), ==, 0);
#endif

				CDeferredStdInLoop Loop;
				auto *pOldLoop = NSys::fg_GetThreadIoLoop();
				auto RestoreLoop = g_OnScopeExit / [&]
					{
						Loop.f_CompleteRemovals();
						NSys::fg_SetThreadIoLoop(pOldLoop);
					}
				;
				NSys::fg_SetThreadIoLoop(&Loop);
				auto fParams = []
					{
						auto Params = NProcess::CStdInReaderParams::fs_Create([](NProcess::EStdInReaderOutputType, NStr::CStr const &) {});
						Params.m_Flags |= NProcess::EStdInReaderFlag_VirtualTerminalInput;
						return Params;
					}
				;

				NStorage::TCUniquePointer<NProcess::CStdInReader> First = fg_Construct(fParams());
				First.f_Clear();
				NStorage::TCUniquePointer<NProcess::CStdInReader> Second = fg_Construct(fParams());
				DMibExpect(Loop.m_nRegistrations, ==, 1);
				Loop.f_CompleteRemovals();
				DMibExpect(Loop.m_nRegistrations, ==, 2);

#ifdef DPlatformFamily_Windows
				DWORD CurrentMode = 0;
				DMibAssertTrue(GetConsoleMode(Input, &CurrentMode) != 0);
				DMibExpect(CurrentMode & (ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT), ==, 0);
				DMibExpect(CurrentMode & ENABLE_MOUSE_INPUT, !=, 0);
#else
				DMibExpect(fcntl(0, F_GETFL) & O_NONBLOCK, !=, 0);
				termios CurrentMode{};
				DMibAssert(tcgetattr(0, &CurrentMode), ==, 0);
				DMibExpect(CurrentMode.c_lflag & (ICANON | ECHO), ==, 0);
#endif
				Second.f_Clear();
				Loop.f_CompleteRemovals();
				DMibTestPath("Restored");

#ifdef DPlatformFamily_Windows
				DMibAssertTrue(GetConsoleMode(Input, &CurrentMode) != 0);
				DMibExpect(CurrentMode, ==, OriginalMode);
#else
				DMibExpect(fcntl(0, F_GETFL), ==, OriginalFlags);
				DMibAssert(tcgetattr(0, &CurrentMode), ==, 0);
				// Canonical-mode restoration may set the kernel-managed PENDIN flag.
				DMibExpect(CurrentMode.c_lflag & ~PENDIN, ==, OriginalMode.c_lflag & ~PENDIN);
				DMibExpect(CurrentMode.c_iflag, ==, OriginalMode.c_iflag);
				DMibExpect(CurrentMode.c_cflag, ==, OriginalMode.c_cflag);
#endif
			};
		}
	};

	DMibTestRegister(CStdInReopen_Tests, Malterlib::Process);
}
