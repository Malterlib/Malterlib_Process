// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Concurrency/ThreadSafeQueue>
#include <Mib/Process/Platform>

namespace
{

	class CProcessInfo_Tests : public CTest
	{
	public:

		CProcessInfo_Tests()
		{
		}


		void f_DoTests()
		{
			DMibTestSuite("WaitForTermination")
			{
				// A process can install termination handlers one after another, as each command of a command line client does
				umint nInstalled = 0;
				for (umint i = 0; i < 3; ++i)
				{
					auto Handler = NMib::NProcess::NPlatform::fg_Process_WaitForTermination([] {});
					if (Handler)
						++nInstalled;
				}
				DMibExpect(nInstalled, ==, umint(3));
			};

			DMibTestSuite("VersionInfo")
			{

				NMib::NProcess::CVersionInfo VersionInfo;
				NMib::NProcess::NPlatform::fg_Process_GetVersionInfo(NMib::NFile::CFile::fs_GetProgramPath(), VersionInfo);

				DMibTest(DMibExpr(VersionInfo.m_Major) == DMibExpr(DProductVersionMajor));
				DMibTest(DMibExpr(VersionInfo.m_Minor) == DMibExpr(DProductVersionMinor));
				DMibTest(DMibExpr(VersionInfo.m_Revision) == DMibExpr(DProductVersionRevision));
				DMibTest(DMibExpr(VersionInfo.m_MinorRevision) == DMibExpr(0));
				DMibTest(DMibExpr(VersionInfo.m_Branch) == DMibExpr(DMalterlibBranch));
//				DMibTrace("BuildTime: {}\r\n", fg_GetFullTimeStr(VersionInfo.m_BuildTime));

			};
		}
	};

	DMibTestRegister(CProcessInfo_Tests, Malterlib::Process);

}

