// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Process/ProcessLaunch>
#include <Mib/Test/Test>
#include <Mib/File/File>
#include <Mib/Encoding/Json>
#include <Mib/Cryptography/RandomID>
#include <Mib/Time/Stopwatch>
#include <Mib/Process/ProcessLaunchActor>
#include <Mib/Concurrency/AsyncDestroy>

#ifndef DPlatformFamily_Windows
	#include <sys/resource.h>
#endif

namespace
{
	using namespace NMib;
	using namespace NMib::NTest;
	using namespace NMib::NTime;
	using namespace NMib::NProcess;
	using namespace NMib::NFile;
	using namespace NMib::NStr;
	using namespace NMib::NContainer;
	using namespace NMib::NCryptography;
	using namespace NMib::NEncoding;

	class CProcessLaunchFeatures_Tests : public CTest
	{
	public:

		CProcessLaunchFeatures_Tests()
		{
		}

		static TCVector<CStr> fs_GetTestGroups()
		{
			auto Groups = NMib::NTest::fg_TestGetCurrentGroups();

			TCVector<CStr> Params;

			if (!Groups.f_IsEmpty())
			{
				Params.f_Insert("--groups");

				CJsonSorted Json = EJsonType_Array;
				for (auto iGroup = Groups.f_GetIterator(); iGroup; ++iGroup)
					Json.f_Insert(iGroup.f_GetKey());
				Params.f_Insert(Json.f_ToString(nullptr));
			}

			return Params;
		}

		void f_TestProcessGroup(bool _bForceFork)
		{
			DMibTestSuite("New Process Group")
			{
				if (fg_TestReportFlags() & ETestReportFlag_ProcessRecursive)
					DMibConOut("{}\n", NProcess::NPlatform::fg_Process_GetCurrentGroupUID());
				else
				{
					TCVector<CStr> RecursiveLaunchParams = {"--test", fg_TestGetCurrentPath(), "--process-recursive", "--logger", "Null"};
					RecursiveLaunchParams.f_Insert(fs_GetTestGroups());

					{
						DMibTestPath("With New Group");
						CProcessLaunchParams LaunchParams;
						LaunchParams.m_bForceFork = _bForceFork;
						LaunchParams.m_bCreateNewProcessGroup = true;

						umint ChildProcessGroup = CProcessLaunch::fs_LaunchTool(CFile::fs_GetProgramPath(), RecursiveLaunchParams, LaunchParams).f_Trim().f_ToInt(umint(0));
						DMibExpect(ChildProcessGroup, !=, NProcess::NPlatform::fg_Process_GetCurrentGroupUID());
					}
					{
						DMibTestPath("Without New Group");
						CProcessLaunchParams LaunchParams;
						LaunchParams.m_bForceFork = _bForceFork;
						LaunchParams.m_bCreateNewProcessGroup = false;

						umint ChildProcessGroup = CProcessLaunch::fs_LaunchTool(CFile::fs_GetProgramPath(), RecursiveLaunchParams, LaunchParams).f_Trim().f_ToInt(umint(0));
						DMibExpect(ChildProcessGroup, ==, NProcess::NPlatform::fg_Process_GetCurrentGroupUID());
					}
				}
			};
		}

		static bool fs_WaitForProcessExit(umint _ProcessID)
		{
			CStopwatch Stopwatch(true);
			while (NProcess::NPlatform::fg_Process_IsRunning(_ProcessID))
			{
				if (Stopwatch.f_GetTime() > 30.0)
					return false;
				NSys::fg_Thread_Sleep(0.05f);
			}

			return true;
		}

		// The launched process starts a grandchild that writes its ID to MibTestTreeIDFile and outlives the launched process,
		// which exits as soon as it has started it
		static void fs_RunOrphanedTreeProcess(TCVector<CStr> const &_RecursiveLaunchParams)
		{
			CStr IDFile = fg_GetSys()->f_GetEnvironmentVariable("MibTestTreeIDFile");
			if (fg_GetSys()->f_GetEnvironmentVariable("MibTestTreeRole") == "Grandchild")
			{
				CFile::fs_WriteStringToFile(IDFile, fg_Format("{}", NProcess::NPlatform::fg_Process_GetCurrentUID()));
				NSys::fg_Thread_Sleep(60.0f);
				return;
			}

			CProcessLaunchParams LaunchParams = CProcessLaunchParams::fs_LaunchExecutable
				(
					CFile::fs_GetProgramPath()
					, _RecursiveLaunchParams
					, CFile::fs_GetCurrentDirectory()
					, nullptr
				)
			;
			LaunchParams.m_bThreaded = true;
			LaunchParams.m_Environment["MibTestTreeRole"] = "Grandchild";
			LaunchParams.m_Environment["MibTestTreeIDFile"] = IDFile;

			CProcessLaunch Launch(LaunchParams, EProcessLaunchCloseFlag_None);
		}

		void f_TestTerminateOrphanedTree(bool _bForceFork)
		{
			DMibTestSuite("Terminate Orphaned Tree")
			{
				TCVector<CStr> RecursiveLaunchParams = {"--test", fg_TestGetCurrentPath(), "--process-recursive", "--logger", "Null"};
				RecursiveLaunchParams.f_Insert(fs_GetTestGroups());

				if (fg_TestReportFlags() & ETestReportFlag_ProcessRecursive)
				{
					fs_RunOrphanedTreeProcess(RecursiveLaunchParams);
					return;
				}

				auto fTest = [&](CStr const &_Case, bool _bClose)
					{
						DMibTestPath(_Case);

						CStr IDFile = CFile::fs_GetProgramDirectory() / fg_Format("TerminateOrphanedTree_{}.txt", fg_RandomID());
						auto RemoveIDFile = g_OnScopeExit / [&]
							{
								if (CFile::fs_FileExists(IDFile))
									CFile::fs_DeleteFile(IDFile);
							}
						;

						CProcessLaunchParams LaunchParams = CProcessLaunchParams::fs_LaunchExecutable
							(
								CFile::fs_GetProgramPath()
								, RecursiveLaunchParams
								, CFile::fs_GetCurrentDirectory()
								, nullptr
							)
						;
						LaunchParams.m_bThreaded = true;
						LaunchParams.m_bForceFork = _bForceFork;
						LaunchParams.m_bCreateNewProcessGroup = true;
						LaunchParams.m_Environment["MibTestTreeIDFile"] = IDFile;

						umint GrandchildID = 0;
						{
							CProcessLaunch Launch(LaunchParams, EProcessLaunchCloseFlag_TerminateProcessTree | EProcessLaunchCloseFlag_BlockOnExit);

							CStopwatch Stopwatch(true);
							while (!GrandchildID && Stopwatch.f_GetTime() < 30.0)
							{
								if (CFile::fs_FileExists(IDFile))
									GrandchildID = CFile::fs_ReadStringFromFile(IDFile).f_Trim().f_ToInt(umint(0));
								NSys::fg_Thread_Sleep(0.05f);
							}
							DMibAssert(GrandchildID, !=, 0);

							DMibAssertTrue(fs_WaitForProcessExit(Launch.f_GetProcessID()));
							DMibExpectTrue(NProcess::NPlatform::fg_Process_IsRunning(GrandchildID));

							if (_bClose)
								NSys::fg_Thread_Sleep(1.0f); // Lets the launch thread finish with the process, so only the close is left to end the tree
							else
								Launch.f_TerminateProcessTree();
						}

						DMibExpectTrue(fs_WaitForProcessExit(GrandchildID));
					}
				;

				fTest("Terminate", false);
				fTest("Close", true);
			};
		}

		void f_TestActorDestroyAfterExit()
		{
			DMibTestSuite("Actor Destroy After Exit") -> NConcurrency::TCFuture<void>
			{
				TCVector<CStr> RecursiveLaunchParams = {"--test", fg_TestGetCurrentPath(), "--process-recursive", "--logger", "Null"};
				RecursiveLaunchParams.f_Insert(fs_GetTestGroups());

				if (fg_TestReportFlags() & ETestReportFlag_ProcessRecursive)
				{
					fs_RunOrphanedTreeProcess(RecursiveLaunchParams);
					co_return {};
				}

				CStr IDFile = CFile::fs_GetProgramDirectory() / fg_Format("ActorDestroyAfterExit_{}.txt", fg_RandomID());
				auto RemoveIDFile = g_OnScopeExit / [IDFile]
					{
						if (CFile::fs_FileExists(IDFile))
							CFile::fs_DeleteFile(IDFile);
					}
				;

				NConcurrency::TCActor<CProcessLaunchActor> LaunchActor = fg_Construct();

				CProcessLaunchActor::CLaunch Launch
					(
						CProcessLaunchParams::fs_LaunchExecutable(CFile::fs_GetProgramPath(), RecursiveLaunchParams, CFile::fs_GetCurrentDirectory(), nullptr)
					)
				;
				Launch.m_Params.m_bCreateNewProcessGroup = true;
				Launch.m_Params.m_Environment["MibTestTreeIDFile"] = IDFile;
				Launch.m_DestructFlags |= EProcessLaunchCloseFlag_TerminateProcessTree;

				NConcurrency::TCPromiseFuturePair<uint32> ExitedPromise;
				Launch.m_Params.m_fOnStateChange = [ExitedPromise = fg_Move(ExitedPromise.m_Promise)](CProcessLaunchStateChangeVariant const &_State, fp64 _TimeSinceStart)
					{
						if (_State.f_GetTypeID() == EProcessLaunchState_Exited)
							ExitedPromise.f_SetResult(_State.f_Get<EProcessLaunchState_Exited>());
					}
				;

				auto LaunchSubscription = co_await LaunchActor(&CProcessLaunchActor::f_Launch, fg_Move(Launch), NConcurrency::fg_CurrentActor());

				co_await fg_Move(ExitedPromise.m_Future).f_Timeout(30.0, "Timed out waiting for the exit");

				umint GrandchildID = 0;
				CStopwatch Stopwatch(true);
				while (!GrandchildID && Stopwatch.f_GetTime() < 30.0)
				{
					if (CFile::fs_FileExists(IDFile))
						GrandchildID = CFile::fs_ReadStringFromFile(IDFile).f_Trim().f_ToInt(umint(0));
					else
						co_await NConcurrency::fg_Timeout(0.05);
				}
				DMibAssert(GrandchildID, !=, 0);
				DMibExpectTrue(NProcess::NPlatform::fg_Process_IsRunning(GrandchildID));

				co_await fg_Move(LaunchActor).f_Destroy();

				Stopwatch.f_Start();
				while (NProcess::NPlatform::fg_Process_IsRunning(GrandchildID) && Stopwatch.f_GetTime() < 30.0)
					co_await NConcurrency::fg_Timeout(0.05);

				DMibExpectFalse(NProcess::NPlatform::fg_Process_IsRunning(GrandchildID));

				co_return {};
			};
		}

		void f_TestActorStopAfterExit()
		{
			DMibTestSuite("Actor Stop After Exit") -> NConcurrency::TCFuture<void>
			{
				NConcurrency::TCActor<CProcessLaunchActor> LaunchActor = fg_Construct();
				auto DestroyLaunchActor = co_await NConcurrency::fg_AsyncDestroy(LaunchActor);

				CProcessLaunchActor::CLaunch Launch
					(
						CProcessLaunchParams::fs_LaunchExecutable(CFile::fs_GetProgramPath(), {"--std-out-exit", "5"}, CFile::fs_GetCurrentDirectory(), nullptr)
					)
				;

				NConcurrency::TCPromiseFuturePair<uint32> ExitedPromise;
				Launch.m_Params.m_fOnStateChange = [ExitedPromise = fg_Move(ExitedPromise.m_Promise)](CProcessLaunchStateChangeVariant const &_State, fp64 _TimeSinceStart)
					{
						if (_State.f_GetTypeID() == EProcessLaunchState_Exited)
							ExitedPromise.f_SetResult(_State.f_Get<EProcessLaunchState_Exited>());
					}
				;

				auto LaunchSubscription = co_await LaunchActor(&CProcessLaunchActor::f_Launch, fg_Move(Launch), NConcurrency::fg_CurrentActor());

				uint32 ExitCode = co_await fg_Move(ExitedPromise.m_Future).f_Timeout(30.0, "Timed out waiting for the exit");
				DMibExpect(ExitCode, ==, 5);

				uint32 TerminateExitCode = co_await LaunchActor(&CProcessLaunchActor::f_TerminateProcessTree).f_Timeout(30.0, "Timed out terminating after the exit");
				DMibExpect(TerminateExitCode, ==, 5);

				co_return {};
			};
		}

#ifndef DPlatformFamily_Windows
		void f_TestCloseTreeHoldingOutput(bool _bForceFork)
		{
			DMibTestSuite("Close Tree Holding Output")
			{
				// The shell exits at once, and the background process that ignores SIGTERM keeps the output open
				CProcessLaunchParams LaunchParams = CProcessLaunchParams::fs_LaunchExecutable
					(
						"/bin/sh"
						, {"-c", "trap '' TERM; /bin/sleep 120 & exit 0"}
						, CFile::fs_GetCurrentDirectory()
						, nullptr
					)
				;
				LaunchParams.m_bThreaded = true;
				LaunchParams.m_bForceFork = _bForceFork;
				LaunchParams.m_bCreateNewProcessGroup = true;

				CStopwatch Stopwatch(true);
				{
					CProcessLaunch Launch(LaunchParams, EProcessLaunchCloseFlag_TerminateProcessTree | EProcessLaunchCloseFlag_BlockOnExit);
					DMibAssertTrue(fs_WaitForProcessExit(Launch.f_GetProcessID()));

					// Lets the launch thread reap the shell and wait for the end of the output, which is where the close has to reach it
					NSys::fg_Thread_Sleep(1.0f);
				}

				DMibExpect(Stopwatch.f_GetTime(), <, 60.0);
			};
		}
#endif

		void f_TestRunAs()
		{
			DMibTestSuite(CTestCategory("Run As") << CTestGroup("SuperUser"))
			{
				if (fg_TestReportFlags() & ETestReportFlag_ProcessRecursive)
				{
					DMibConOut("User: {} Group: {}\n", NSys::fg_UserManagement_GetProcessRealUserName(), NSys::fg_UserManagement_GetProcessRealGroupName());
					return;
				}
				CStr RunAsPath = fg_TestGetCurrentPath();
				for (umint i = 0; i < 4; ++i)
				{
					bool bForceFork = (i & 1) != 0;
					bool bUserSession = (i & 2) != 0;

					DMibTestPath(bForceFork ? "Fork" : "No Fork");
					{
						DMibTestPath(bUserSession ? "User Session" : "No User Session");

						CStr Tmp;

						CStr TestGroup = "_MibProcessTestGroup{}"_f << (bForceFork ? "F" : "N") << (bUserSession ? "S" : "");
						CStr TestUser = "_MibProcessTestUser{}"_f << (bForceFork ? "F" : "N") << (bUserSession ? "S" : "");

						CStrSecure TestPassword = fg_RandomID() + "1aA@";

						auto fDeleteUser = [&]
							{
								if (NMib::NSys::fg_UserManagement_UserExists(TestUser, Tmp))
									NMib::NSys::fg_UserManagement_DeleteUser(TestUser);
							}
						;
						auto fDeleteGroup = [&]
							{
								if (NMib::NSys::fg_UserManagement_GroupExists(TestGroup, Tmp))
									NMib::NSys::fg_UserManagement_DeleteGroup(TestGroup);
							}
						;
						fDeleteUser();
						fDeleteGroup();

						CStr ExecutableToLaunch = CFile::fs_GetProgramPath();

						auto Attributes = CFile::fs_GetAttributes(ExecutableToLaunch);
						auto NewAttributes = Attributes | EFileAttrib_EveryoneExecute;
						if (NewAttributes != Attributes)
							CFile::fs_SetAttributes(ExecutableToLaunch, NewAttributes | EFileAttrib_UnixAttributesValid);

						CStr CreatedGID;
						NMib::NSys::fg_UserManagement_CreateGroup(TestGroup, CreatedGID);

						CStr HomeDir = CFile::fs_GetProgramDirectory() / "homes" / TestUser;

						CFile::fs_CreateDirectory(HomeDir);
	#ifdef DPlatformFamily_macOS
						// Workaround flaky API
						for (umint iRetry = 0; ; ++iRetry)
						{
							if (iRetry == 10)
							{
								CFile::fs_SetGroup(HomeDir, TestGroup);
								break;
							}
							else
							{
								try
								{
									CFile::fs_SetGroup(HomeDir, TestGroup);
									break;
								}
								catch (NException::CException const &)
								{
								}
							}

							NSys::fg_Thread_Sleep(10_ms);
						}
	#else
						CFile::fs_SetGroup(HomeDir, TestGroup);
	#endif

						CStr CreatedUID;
						fg_UserManagement_CreateUser
							(
								TestGroup
								, TestUser
								, TestPassword
								, "Test FullName"
								, HomeDir
								, CreatedUID
								, NMib::NSys::EUserManagementCreateUserFlag_None
							)
						;

						CFile::fs_SetOwner(HomeDir, TestUser);

						TCVector<CStr> RecursiveLaunchParams = {"--test", RunAsPath, "--process-recursive", "--logger", "Null"};
						RecursiveLaunchParams.f_Insert(fs_GetTestGroups());

						CStr ExpectedOutput = "User: {} Group: {}"_f << TestUser << TestGroup;

						{
							DMibTestPath("Without Launch As");
							CProcessLaunchParams LaunchParams;
							LaunchParams.m_bForceFork = bForceFork;
							LaunchParams.m_bLaunchInUserSession = bUserSession;

							CStr Output = CProcessLaunch::fs_LaunchTool(ExecutableToLaunch, RecursiveLaunchParams, LaunchParams).f_Trim();
							DMibExpect(Output, !=, ExpectedOutput);
						}
						{
							DMibTestPath("With Launch As");
							CProcessLaunchParams LaunchParams;
							LaunchParams.m_bForceFork = bForceFork;
							LaunchParams.m_bLaunchInUserSession = bUserSession;
							LaunchParams.m_RunAsUser = TestUser;
							LaunchParams.m_RunAsGroup = TestGroup;

							CStr Output = CProcessLaunch::fs_LaunchTool(ExecutableToLaunch, RecursiveLaunchParams, LaunchParams).f_Trim();
							DMibExpect(Output, ==, ExpectedOutput);
						}

						fDeleteUser();
						fDeleteGroup();
					}
				}
			};
		}

		void f_TestPriority(bool _bForceFork)
		{
			DMibTestSuite("Priority")
			{
				if (fg_TestReportFlags() & ETestReportFlag_ProcessRecursive)
					DMibConOut("{}\n", int32(NProcess::NPlatform::fg_Process_GetPriority()));
				else
				{
					TCVector<CStr> RecursiveLaunchParams = {"--test", fg_TestGetCurrentPath(), "--process-recursive", "--logger", "Null"};
					RecursiveLaunchParams.f_Insert(fs_GetTestGroups());


					{
						DMibTestPath("Without Priority");
						CProcessLaunchParams LaunchParams;
						LaunchParams.m_bForceFork = _bForceFork;

						int32 ThisPriority = NProcess::NPlatform::fg_Process_GetPriority();

						int32 Priority = CProcessLaunch::fs_LaunchTool(CFile::fs_GetProgramPath(), RecursiveLaunchParams, LaunchParams).f_Trim().f_ToInt(int32(EExecutionPriority_Normal));
						DMibExpect(Priority, ==, ThisPriority);
					}
					{
						DMibTestPath("With Priority");
						CProcessLaunchParams LaunchParams;
						LaunchParams.m_bForceFork = _bForceFork;
						LaunchParams.m_LaunchPriority = EExecutionPriority_Lowest;

						int32 Priority = CProcessLaunch::fs_LaunchTool(CFile::fs_GetProgramPath(), RecursiveLaunchParams, LaunchParams).f_Trim().f_ToInt(int32(EExecutionPriority_Normal));
						DMibExpect(Priority, ==, EExecutionPriority_Lowest);
					}
				}
			};
		}

#ifndef DPlatformFamily_Windows
		void f_TestLimits(bool _bForceFork)
		{
			DMibTestSuite("Limits")
			{
				if (fg_TestReportFlags() & ETestReportFlag_ProcessRecursive)
				{
					rlimit Limits;
					if (!getrlimit(RLIMIT_FSIZE, &Limits))
					{
						DMibConOut("Current: {} Max: {}\n", Limits.rlim_cur, Limits.rlim_max);
						return;
					}

					DMibConOut("Error getting rlimit\n");
				}
				else
				{
					TCVector<CStr> RecursiveLaunchParams = {"--test", fg_TestGetCurrentPath(), "--process-recursive", "--logger", "Null"};
					RecursiveLaunchParams.f_Insert(fs_GetTestGroups());

					umint TryLimitCur = 4 * 1024 * 1024;
					umint TryLimitMax = 8 * 1024 * 1024;

					CStr ExpectedOutput = "Current: {} Max: {}"_f << TryLimitCur << TryLimitMax;
					{
						DMibTestPath("Without Limits");
						CProcessLaunchParams LaunchParams;
						LaunchParams.m_bForceFork = _bForceFork;

						CStr Output = CProcessLaunch::fs_LaunchTool(CFile::fs_GetProgramPath(), RecursiveLaunchParams, LaunchParams).f_Trim();
						DMibExpect(Output, !=, ExpectedOutput);
					}
					{
						DMibTestPath("With Limits");
						CProcessLaunchParams LaunchParams;

						LaunchParams.m_bForceFork = _bForceFork;

						auto &Limit = LaunchParams.m_Limits[EProcessLimit_FileSize];
						Limit.m_Value = TryLimitCur;
						Limit.m_MaxValue = TryLimitMax;

						CStr Output = CProcessLaunch::fs_LaunchTool(CFile::fs_GetProgramPath(), RecursiveLaunchParams, LaunchParams).f_Trim();
						DMibExpect(Output, ==, ExpectedOutput);
					}
				}
			};
		}
#endif

		void f_DoTests()
		{
			DMibTestCategory("No Fork")
			{
				f_TestProcessGroup(false);
				f_TestTerminateOrphanedTree(false);
				f_TestActorStopAfterExit();
				f_TestActorDestroyAfterExit();
				f_TestPriority(false);
#ifndef DPlatformFamily_Windows
				f_TestLimits(false);
				f_TestCloseTreeHoldingOutput(false);
#endif
			};

#ifndef DPlatformFamily_Windows
			DMibTestCategory("Fork")
			{
				f_TestProcessGroup(false);
				f_TestTerminateOrphanedTree(true);
				f_TestPriority(false);
				f_TestLimits(false);
				f_TestCloseTreeHoldingOutput(true);
			};
#endif
#ifndef DPlatformFamily_Windows
			f_TestRunAs();
#endif
		}
	};

	DMibTestRegister(CProcessLaunchFeatures_Tests, Malterlib::Process);
}
