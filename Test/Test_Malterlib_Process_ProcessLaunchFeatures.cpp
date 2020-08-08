// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Process/ProcessLaunch>
#include <Mib/Test/Test>
#include <Mib/File/File>
#include <Mib/Encoding/JSON>
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

				CJSON JSON = EJSONType_Array;
				for (auto iGroup = Groups.f_GetIterator(); iGroup; ++iGroup)
					JSON.f_Insert(iGroup.f_GetKey());
				Params.f_Insert(JSON.f_ToString(nullptr));
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

		void f_TestRunAs(bool _bForceFork)
		{
			DMibTestSuite(CTestCategory("Run As") << CTestGroup("SuperUser"))
			{
				if (fg_TestReportFlags() & ETestReportFlag_ProcessRecursive)
					DMibConOut2("User: {} Group: {}\n", NSys::fg_UserManagement_GetProcessRealUserName(), NSys::fg_UserManagement_GetProcessRealGroupName());
				else
				{
					CStr Tmp;

					CStr TestGroup = "_MibProcessTestGroup";
					CStr TestUser = "_MibProcessTestUser";

					CStrSecure TestPassword = fg_RandomID() + "1aA@";

					if (NMib::NSys::fg_UserManagement_UserExists(TestUser, Tmp))
						NMib::NSys::fg_UserManagement_DeleteUser(TestUser);

					if (NMib::NSys::fg_UserManagement_GroupExists(TestGroup, Tmp))
						NMib::NSys::fg_UserManagement_DeleteGroup(TestGroup);

					CStr CreatedGID;
					NMib::NSys::fg_UserManagement_CreateGroup(TestGroup, CreatedGID);

					CStr CreatedUID;
					fg_UserManagement_CreateUser
						(
							TestGroup
							, TestUser
							, TestPassword
							, "Test FullName"
							, NMib::NFile::CFile::fs_GetProgramDirectory()
							, CreatedUID
							, NMib::NSys::EUserManagementCreateUserFlag_None
						)
					;

					TCVector<CStr> RecursiveLaunchParams = {"--test", fg_TestGetCurrentPath(), "--process-recursive", "--logger", "Null"};
					RecursiveLaunchParams.f_Insert(fs_GetTestGroups());

					CStr ExpectedOutput = "User: {} Group: {}"_f << TestUser << TestGroup;

					{
						DMibTestPath("Without Launch As");
						CProcessLaunchParams LaunchParams;
						LaunchParams.m_bForceFork = _bForceFork;

						CStr Output = CProcessLaunch::fs_LaunchTool(CFile::fs_GetProgramPath(), RecursiveLaunchParams, LaunchParams).f_Trim();
						DMibExpect(Output, !=, ExpectedOutput);
					}
					{
						DMibTestPath("With Launch As");
						CProcessLaunchParams LaunchParams;
						LaunchParams.m_bForceFork = _bForceFork;
						LaunchParams.m_RunAsUser = TestUser;
						LaunchParams.m_RunAsGroup = TestGroup;

						CStr Output = CProcessLaunch::fs_LaunchTool(CFile::fs_GetProgramPath(), RecursiveLaunchParams, LaunchParams).f_Trim();
						DMibExpect(Output, ==, ExpectedOutput);
					}

					NMib::NSys::fg_UserManagement_DeleteUser(TestUser);
					NMib::NSys::fg_UserManagement_DeleteGroup(TestGroup);
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

						int32 Priority = CProcessLaunch::fs_LaunchTool(CFile::fs_GetProgramPath(), RecursiveLaunchParams, LaunchParams).f_Trim().f_ToInt(int32(EExecutionPriority_Normal));
						DMibExpect(Priority, ==, EExecutionPriority_Normal);
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
				f_TestRunAs(false);
				f_TestLimits(false);
#endif
			};

#ifndef DPlatformFamily_Windows
			DMibTestCategory("Fork")
			{
				f_TestProcessGroup(false);
				f_TestPriority(false);
				f_TestRunAs(false);
				f_TestLimits(false);
			};
#endif
		}
	};

	DMibTestRegister(CProcessLaunchFeatures_Tests, Malterlib::Process);
}
