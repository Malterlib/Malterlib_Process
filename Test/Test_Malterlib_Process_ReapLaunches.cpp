// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Test/Exception>
#include <Mib/Process/VirtualProcessLaunch>
#include <Mib/Concurrency/ThreadSafeQueue>
#include <Mib/File/File>

namespace
{
	using namespace NMib;
	using namespace NMib::NProcess;
	using namespace NMib::NStr;

	struct CLaunchLifetime
	{
		umint m_nClosed = 0;
		umint m_nDestroyed = 0;
		bool m_bJoined = false;
	};

	struct CTrackedLaunch : CVirtualProcessLaunch_Default
	{
		CTrackedLaunch(CProcessLaunchParams const &_Params, EProcessLaunchCloseFlag _Flags, CLaunchLifetime *_pLifetime)
			: CVirtualProcessLaunch_Default(_Params, _Flags)
			, m_pLifetime(_pLifetime)
		{
		}

		~CTrackedLaunch() override
		{
			++m_pLifetime->m_nDestroyed;
		}

		void f_Close(EProcessLaunchCloseFlag _Flags) override
		{
			++m_pLifetime->m_nClosed;
			m_pLifetime->m_bJoined = (_Flags & EProcessLaunchCloseFlag_BlockOnExit) != 0;

			CVirtualProcessLaunch_Default::f_Close(_Flags);
		}

		CLaunchLifetime *m_pLifetime;
	};

	struct CReapLaunches_Tests : NTest::CTest
	{
		void f_DoTests()
		{
			DMibTestSuite("Lifecycle")
			{
				for (bool bFail : {false, true})
				{
					DMibTestPath(bFail ? "LaunchFailed" : "Exited");

					CLaunchLifetime Lifetime;
					NContainer::TCThreadSafeQueue<NFunction::TCFunction<void ()>> Dispatch;
					NThread::CEventAutoReset Changed;
					CProcessLaunchHandler Handler;
					auto Cleanup = g_OnScopeExit / [&]
						{
							Handler.f_TerminateAll(true);
						}
					;

					bool bCompleted = false;
					bool bLaunchFailed = false;
					CProcessLaunchParams Params;
					Params.m_Target = bFail ? NFile::CFile::fs_GetProgramDirectory() / "missing-reap-test-executable" : NFile::CFile::fs_GetProgramPath();
					Params.m_Parameters = "--std-out-exit 0";
					Params.m_fDispatcher = [&](NFunction::TCFunction<void ()> const &_Function)
						{
							Dispatch.f_Push(_Function);
							Changed.f_Signal();
						}
					;

					Params.m_fOnStateChange = [&](CProcessLaunchStateChangeVariant const &_State, fp64)
						{
							if (_State.f_GetTypeID() == EProcessLaunchState_Exited || _State.f_GetTypeID() == EProcessLaunchState_LaunchFailed)
							{
								bLaunchFailed = _State.f_GetTypeID() == EProcessLaunchState_LaunchFailed;
								bCompleted = true;
							}
						}
					;

					Handler.f_AddLaunch
						(
							Params, false
							, [&](CProcessLaunchParams const &_Params, EProcessLaunchCloseFlag _Flags) -> NStorage::TCUniquePointer<CVirtualProcessLaunch>
							{
								return fg_Construct<CTrackedLaunch>(_Params, _Flags, &Lifetime);
							}
						)
					;

					{
						DMibTestPath("PendingDispatch");

						Handler.f_ReapCompleted();

						DMibExpect(Lifetime.m_nDestroyed, ==, umint(0));
					}

					NTime::CStopwatch WaitTime(true);
					while (!bCompleted && WaitTime.f_GetTime() < 30.0 * NTest::gc_TimeoutMultiplier)
					{
						while (auto Entry = Dispatch.f_Pop())
							(*Entry)();

						if (!bCompleted)
							Changed.f_WaitTimeout(0.1);
					}

					DMibAssertTrue(bCompleted);
					DMibExpect(bLaunchFailed, ==, bFail);
					DMibExpect(Lifetime.m_nDestroyed, ==, umint(0));

					Handler.f_ReapCompleted();

					DMibExpect(Lifetime.m_nClosed, ==, umint(1));
					DMibExpectTrue(Lifetime.m_bJoined);
					DMibExpect(Lifetime.m_nDestroyed, ==, umint(1));
					DMibExpectTrue(Handler.f_GetFirstNotDone() == nullptr);

					{
						DMibTestPath("RepeatedReap");

						Handler.f_ReapCompleted();

						DMibExpect(Lifetime.m_nDestroyed, ==, umint(1));
					}
				}
			};
		}
	};
}

DMibTestRegister(CReapLaunches_Tests, Malterlib::Process);
