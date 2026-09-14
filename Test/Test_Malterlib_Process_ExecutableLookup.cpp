// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Test/Test>
#include <Mib/File/File>
#include <Mib/Process/ProcessLaunch>
#include "../Source/Malterlib_Process_Platform.h"

namespace
{
	using namespace NMib;
	using namespace NMib::NStr;

	struct CExecutableLookup_Tests : NTest::CTest
	{
		void f_DoTests()
		{
#ifdef DPlatformFamily_Windows
			DMibTestSuite("LongWindowsPath")
			{
				fp_TestLookup(true);
			};

			DMibTestSuite("WindowsExtensions")
			{
				fp_TestLookup(false);
			};
#endif
			DMibTestSuite("NoImplicitCurrentDirectory")
			{
				fp_TestCurrentDirectory();
			};
		}

	private:
		void fp_TestCurrentDirectory()
		{
			using NFile::CFile;
			using NProcess::NPlatform::fg_FindExecutable;

			CStr Directory = CFile::fs_GetProgramDirectory() / "ProcessTests" / NTest::fg_TestGetCurrentPath().f_RemovePrefix("Malterlib/Process/");
			CStr Working = Directory / "working";
			CStr Trusted = Directory / "trusted";
			CStr ShadowName = "cwd-shadow" + CFile::mc_ExecutableExtension;
			CStr LocalName = "cwd-only" + CFile::mc_ExecutableExtension;
#ifdef DPlatformFamily_Windows
			CStr Source = fg_FindExecutable("cmd.exe");
			CStr Separator = ";";
			NContainer::TCVector<CStr> Arguments{"/d", "/c", "exit", "23"};
#else
			CStr Separator = ":";
			NContainer::TCVector<CStr> Arguments;
#endif
			NTest::fg_TestAddCleanupPath(Directory);
			CFile::fs_CreateDirectory(Working);
			CFile::fs_CreateDirectory(Trusted);
			CFile::fs_CreateDirectory(Working / "relative");
			for (auto const &File : {Working / ShadowName, Trusted / ShadowName, Working / LocalName, Working / "relative" / ShadowName})
			{
#ifdef DPlatformFamily_Windows
				CFile::fs_CopyFile(Source, File);
#else
				// macOS can reject relocated platform binaries; the fixture only needs a known exit status.
				CFile::fs_WriteStringToFile(File, "#!/bin/sh\nexit 23\n", false);
				CFile::fs_SetAttributes(File, CFile::fs_GetAttributes(File) | NFile::EFileAttrib_Executable);
#endif
			}
#ifdef DPlatformFamily_Windows
			CFile::fs_CopyFile(Source, Working / "cwd-shadow.com");
#endif

			CStr OldPath = fg_GetSys()->f_GetEnvironmentVariable("PATH");
			CStr OldDirectory = CFile::fs_GetCurrentDirectory();
			auto Restore = g_OnScopeExit / [&]
				{
					CFile::fs_SetCurrentDirectory(OldDirectory);
					fg_GetSys()->f_SetEnvironmentVariable("PATH", OldPath);
				}
			;
			CFile::fs_SetCurrentDirectory(Working);
			fg_GetSys()->f_SetEnvironmentVariable("PATH", Trusted);

			DMibExpect(fg_FindExecutable(ShadowName).f_CmpNoCase(Trusted / ShadowName), ==, 0);
#ifdef DPlatformFamily_Windows
			DMibExpect(CFile::fs_GetFile(fg_FindExecutable("cwd-shadow")), ==, "cwd-shadow.exe");
#endif
			DMibExpect(fg_FindExecutable("./" + ShadowName).f_CmpNoCase(Working / ShadowName), ==, 0);
			DMibExpect(fg_FindExecutable(LocalName, false), ==, LocalName);

			{
				DMibTestPath("EmptyEntries");
				fg_GetSys()->f_SetEnvironmentVariable("PATH", Separator + Trusted + Separator + Separator);

				DMibExpect(fg_FindExecutable(ShadowName).f_CmpNoCase(Trusted / ShadowName), ==, 0);
			}

			auto fCheckRelativeEntry = [&](CStr const &_Case, CStr const &_Entry)
				{
					DMibTestPath(_Case);
					fg_GetSys()->f_SetEnvironmentVariable("PATH", _Entry + Separator + Trusted);

					DMibExpect(fg_FindExecutable(ShadowName).f_CmpNoCase(Trusted / ShadowName), ==, 0);
				}
			;

			fCheckRelativeEntry("DotEntry", ".");
			fCheckRelativeEntry("RelativeDirectory", "relative");
#ifdef DPlatformFamily_Windows
			CStr Drive = CFile::fs_GetDrive(Working);
			fCheckRelativeEntry("DriveRelative", Drive + ".");
			fCheckRelativeEntry("RootRelative", Working.f_Extract(Drive.f_GetLen()));
			fCheckRelativeEntry("AbsoluteDriveRoot", Drive + "/");
#endif

			fg_GetSys()->f_SetEnvironmentVariable("PATH", Separator + Separator);
			{
				DMibTestPath("MissCannotLaunchLocally");
				CStr StdOut;
				CStr StdErr;
				uint32 ExitCode = 1;
				bool bLaunched = NProcess::CProcessLaunch::fs_LaunchBlock(LocalName, Arguments, StdOut, StdErr, ExitCode);

				DMibExpectFalse(bLaunched);
			}

			{
				DMibTestPath("ExplicitLocalLaunch");
				CStr StdOut;
				CStr StdErr;
				uint32 ExitCode = 1;
				bool bLaunched = NProcess::CProcessLaunch::fs_LaunchBlock("./" + LocalName, Arguments, StdOut, StdErr, ExitCode);

				DMibAssertTrue(bLaunched);
				DMibExpect(ExitCode, ==, 23);
			}
		}

#ifdef DPlatformFamily_Windows
		void fp_TestLookup(bool _bLongPath)
		{
			using NFile::CFile;
			using NProcess::NPlatform::fg_FindExecutable;

			if (NTest::fg_TestReportFlags() & NTest::ETestReportFlag_ProcessRecursive)
			{
				CStr Expected = fg_GetSys()->f_GetEnvironmentVariable("ExecutableLookupExpected");
				DMibExpect(CFile::fs_GetProgramPath().f_CmpNoCase(Expected), ==, 0);

				return;
			}

			CStr TestPath = NTest::fg_TestGetCurrentPath();
			CStr Directory = CFile::fs_GetProgramDirectory() / "ProcessTests" / TestPath.f_RemovePrefix("Malterlib/Process/");
			CStr First = Directory / "first";
			CStr Second = Directory / "second";
			CStr Local = Directory / "launch";
			NTest::fg_TestAddCleanupPath(Directory);
			CFile::fs_CreateDirectory(First);
			CFile::fs_CreateDirectory(Second);
			CFile::fs_CreateDirectory(Local);
			CStr CommandInterpreter = fg_FindExecutable("cmd.exe");
			for (auto const *pName : {"onlyexe.exe", "onlycom.com", "prefer.com", "prefer.exe", "order.exe", "same.exe"})
				CFile::fs_CopyFile(CommandInterpreter, First / pName);
			CFile::fs_CopyFile(CommandInterpreter, Second / "order.com");
			CFile::fs_CopyFile(CommandInterpreter, Second / "same.exe");
			// The copied test executable needs its runtime DLLs even when PATH and the working directory change.
			auto RuntimeLibraries = CFile::fs_FindFiles(CFile::fs_GetProgramDirectory() / "*.dll", NFile::EFileAttrib_File);
			for (auto const &Executable : {First / "choice.com", Local / "choice.exe"})
			{
				CFile::fs_CopyFile(CFile::fs_GetProgramPath(), Executable);
				for (auto const &Library : RuntimeLibraries)
					CFile::fs_CopyFile(Library, CFile::fs_GetPath(Executable) / CFile::fs_GetFile(Library));
			}

			CStr OldPath = fg_GetSys()->f_GetEnvironmentVariable("PATH");
			auto Restore = g_OnScopeExit / [&]
				{
					fg_GetSys()->f_SetEnvironmentVariable("PATH", OldPath);
				}
			;
			CStr Path;
			if (_bLongPath)
			{
				while (Path.f_GetLen() < 4096)
					Path += Directory.f_ReplaceChar('/', '\\') + "\\missing;";
			}
			Path += (Second + " ").f_ReplaceChar('/', '\\') + ";";
			Path += "\"" + Second.f_ReplaceChar('/', '\\') + "\";";
			Path += Second.f_ReplaceChar('/', '\\') + "\";";
			Path += (First + ".").f_ReplaceChar('/', '\\') + ";" + Second.f_ReplaceChar('/', '\\') + ";" + (First / ".").f_ReplaceChar('/', '\\');
			fg_GetSys()->f_SetEnvironmentVariable("PATH", Path);

			DMibExpect(CFile::fs_GetFile(fg_FindExecutable("onlyexe")), ==, "onlyexe.exe");
			DMibExpect(CFile::fs_GetFile(fg_FindExecutable("onlycom")), ==, "onlycom.com");
			DMibExpect(CFile::fs_GetFile(fg_FindExecutable("prefer")), ==, "prefer.com");
			DMibExpect(CFile::fs_GetFile(fg_FindExecutable("prefer.exe")), ==, "prefer.exe");
			DMibExpect(CFile::fs_GetFile(fg_FindExecutable("order")), ==, "order.exe");
			DMibExpect(fg_FindExecutable("same.exe").f_CmpNoCase(First / "same.exe"), ==, 0);
			DMibExpect(fg_FindExecutable(First / "prefer").f_CmpNoCase(First / "prefer.com"), ==, 0);
			DMibExpect(fg_FindExecutable(First / "prefer.exe", false), ==, First / "prefer.exe");
			DMibExpect(fg_FindExecutable("prefer", false), ==, "prefer");

			auto fLaunch = [&](CStr const &_Case, CStr const &_LocalPath, CStr const &_Expected)
				{
					DMibTestPath(_Case);
					NProcess::CProcessLaunchParams Params;
					Params.m_Environment["Path"] = _LocalPath;
					Params.m_Environment["ExecutableLookupExpected"] = _Expected;
					CStr StdOut;
					CStr StdErr;
					uint32 ExitCode = 1;
					bool bLaunched = NProcess::CProcessLaunch::fs_LaunchBlock
						(
							"choice"
							, {"--test", TestPath, "--process-recursive"}
							, StdOut, StdErr, ExitCode, Params
						)
					;

					if (ExitCode)
						DMibConOut("{}{}", StdOut, StdErr);
					DMibAssertTrue(bLaunched);
					DMibExpect(ExitCode, ==, 0);
				}
			;

			fLaunch("LaunchPathFirst", Local.f_ReplaceChar('/', '\\'), Local / "choice.exe");
			fLaunch("QuotedLaunchPath", "\"" + Local.f_ReplaceChar('/', '\\') + "\"", First / "choice.com");
			fLaunch("SystemPathFallback", (Directory / "missing").f_ReplaceChar('/', '\\'), First / "choice.com");
			fLaunch("EmptyLaunchPath", "", First / "choice.com");

			{
				CStr OldDirectory = CFile::fs_GetCurrentDirectory();
				auto RestoreDirectory = g_OnScopeExit / [&]
					{
						CFile::fs_SetCurrentDirectory(OldDirectory);
					}
				;
				CFile::fs_SetCurrentDirectory(Local);

				fLaunch("RelativeLaunchPath", ".", First / "choice.com");
			}

			NProcess::CProcessLaunchParams Params;
			Params.m_Environment["PATH"] = Local.f_ReplaceChar('/', '\\');
			CStr StdOut;
			CStr StdErr;
			uint32 ExitCode = 1;
			bool bLaunched = NProcess::CProcessLaunch::fs_LaunchBlock("onlycom", {"/d", "/c", "exit", "23"}, StdOut, StdErr, ExitCode, Params);

			DMibAssertTrue(bLaunched);
			DMibExpect(ExitCode, ==, 23);
		}
#endif
	};

	DMibTestRegister(CExecutableLookup_Tests, Malterlib::Process);
}
