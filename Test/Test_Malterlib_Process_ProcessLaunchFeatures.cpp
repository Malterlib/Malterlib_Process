// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Process/ProcessLaunch>
#include <Mib/Test/Test>
#include <Mib/File/File>
#include <Mib/Encoding/Json>
#include <Mib/Cryptography/RandomID>

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
					DMibConOut2("{}\n", NProcess::NPlatform::fg_Process_GetCurrentGroupUID());
				else
				{
					TCVector<CStr> RecursiveLaunchParams = {"--test", fg_TestGetCurrentPath(), "--process-recursive", "--logger", "Null"};
					RecursiveLaunchParams.f_Insert(fs_GetTestGroups());

					{
						DMibTestPath("With New Group");
						CProcessLaunchParams LaunchParams;
						LaunchParams.m_bForceFork = _bForceFork;
						LaunchParams.m_bCreateNewProcessGroup = true;

						mint ChildProcessGroup = CProcessLaunch::fs_LaunchTool(CFile::fs_GetProgramPath(), RecursiveLaunchParams, LaunchParams).f_Trim().f_ToInt(mint(0));
						DMibExpect(ChildProcessGroup, !=, NProcess::NPlatform::fg_Process_GetCurrentGroupUID());
					}
					{
						DMibTestPath("Without New Group");
						CProcessLaunchParams LaunchParams;
						LaunchParams.m_bForceFork = _bForceFork;
						LaunchParams.m_bCreateNewProcessGroup = false;

						mint ChildProcessGroup = CProcessLaunch::fs_LaunchTool(CFile::fs_GetProgramPath(), RecursiveLaunchParams, LaunchParams).f_Trim().f_ToInt(mint(0));
						DMibExpect(ChildProcessGroup, ==, NProcess::NPlatform::fg_Process_GetCurrentGroupUID());
					}
				}
			};
		}

		void f_TestRunAs()
		{
			DMibTestSuite(CTestCategory("Run As") << CTestGroup("SuperUser"))
			{
				if (fg_TestReportFlags() & ETestReportFlag_ProcessRecursive)
				{
					DMibConOut2("User: {} Group: {}\n", NSys::fg_UserManagement_GetProcessRealUserName(), NSys::fg_UserManagement_GetProcessRealGroupName());
					return;
				}
				CStr RunAsPath = fg_TestGetCurrentPath();
				for (mint i = 0; i < 4; ++i)
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
						for (mint iRetry = 0; ; ++iRetry)
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
					DMibConOut2("{}\n", int32(NProcess::NPlatform::fg_Process_GetPriority()));
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
						DMibConOut2("Current: {} Max: {}\n", Limits.rlim_cur, Limits.rlim_max);
						return;
					}

					DMibConOut2("Error getting rlimit\n");
				}
				else
				{
					TCVector<CStr> RecursiveLaunchParams = {"--test", fg_TestGetCurrentPath(), "--process-recursive", "--logger", "Null"};
					RecursiveLaunchParams.f_Insert(fs_GetTestGroups());

					mint TryLimitCur = 4 * 1024 * 1024;
					mint TryLimitMax = 8 * 1024 * 1024;

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
				f_TestPriority(false);
#ifndef DPlatformFamily_Windows
				f_TestLimits(false);
#endif
			};

#ifndef DPlatformFamily_Windows
			DMibTestCategory("Fork")
			{
				f_TestProcessGroup(false);
				f_TestPriority(false);
				f_TestLimits(false);
			};
#endif
#ifndef DPlatformFamily_Windows
			f_TestRunAs();
#endif
		}
	};

	DMibTestRegister(CProcessLaunchFeatures_Tests, Malterlib::Process);
}
