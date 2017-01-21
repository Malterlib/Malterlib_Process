// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Test/Exception>

#include <Mib/Concurrency/ThreadSafeQueue>
#include <Mib/Cryptography/UUID>
#include <Mib/Process/StdIn>
#include <Mib/Process/ProcessLaunch>

namespace
{
	using namespace NMib::NTest;
	using namespace NMib::NTime;

	enum EExitResult
	{
		EExitResult_None
		, EExitResult_Exited
		, EExitResult_NotLaunched
	};
	class CStdIn_Tests : public CTest
	{
	public:

		CStdIn_Tests()
		{
		}

		static NMib::NStr::CStr fs_GetTestPath(NMib::NStr::CStr const &_TestPath, NMib::NStr::CStr const &_Test)
		{
			if (_Test.f_IsEmpty())
				return NMib::NStr::CStr::CFormat("--Tests \"{}\" --TestLogger Null --TestResults (All ProcessRecursive)") << _TestPath;
			else
				return NMib::NStr::CStr::CFormat("--Tests \"{}/{}\" --TestLogger Null --TestResults (All ProcessRecursive)") << _TestPath << _Test;
		}

		static NMib::NProcess::CProcessLaunchParams fs_GetLaunchParams(NMib::NStr::CStr const &_Test = "JustExit", NMib::NStr::CStr const &_TestPath = fg_TestGetCurrentPath())
		{
			NMib::NProcess::CProcessLaunchParams Params;
			Params.m_Target = NMib::NFile::CFile::fs_GetProgramPath();		
			Params.m_Parameters = fs_GetTestPath(_TestPath, _Test);
			return Params;
		}

		template <bool t_bForcePolling>
		void f_TestStdIn()
		{
			NMib::NStr::CStr CategoryName;
			if (t_bForcePolling)
				CategoryName = "General";
			else
				CategoryName = "Force polling";

			DMibTestCategory(CategoryName)
			{
				using namespace NMib::NProcess;
				DMibTestSuite("Lifetime")
				{
					NMib::NThread::CMutual InputLock;
					NMib::NStr::CStr Input;
					auto fCreateParams = [&](bool _bExclusive = false)
						{
							auto Params = CStdInReaderParams::fs_Create
								(
									[&](EStdInReaderOutputType _Type, NMib::NStr::CStr const &_Input)
									{
										DMibLock(InputLock);
										Input += _Input;
									}
								)
							;
							Params.m_Flags |= t_bForcePolling ? EStdInReaderFlag_ForcePolling : EStdInReaderFlag_None;
							if (_bExclusive)
								Params.m_Flags |= EStdInReaderFlag_Exclusive;
							return NMib::fg_Move(Params);
						}
					;


					CStdInReaderParams ExclusiveParams = fCreateParams();
					ExclusiveParams.m_Flags |= EStdInReaderFlag_Exclusive;

					{
						CStdInReader Reader0(fCreateParams());
						CStdInReader Reader1(fCreateParams());
					}
					auto fl_CreateNormal
						= [&]
						{
							CStdInReader Reader1(fCreateParams());
						}
					;
					auto fl_CreateExclusive
						= [&]
						{
							CStdInReader Reader1(fCreateParams(true));
						}
					;
					{
						DMibTestPath("Shared");
						CStdInReader Reader0(fCreateParams());
						DMibTest(DMibExpr(TCThrowsException<NMib::NException::CException>()) == DMibLExpr(fl_CreateExclusive()));
						DMibTest(DMibExpr(TCThrowsException<>()) == DMibLExpr(fl_CreateNormal()));
					}
					{
						DMibTestPath("Exclusive");
						CStdInReader Reader0(fCreateParams(true));
						DMibTest(DMibExpr(TCThrowsException<NMib::NException::CException>()) == DMibLExpr(fl_CreateExclusive()));
						DMibTest(DMibExpr(TCThrowsException<NMib::NException::CException>()) == DMibLExpr(fl_CreateNormal()));
					}
	
				};
				DMibTestSuite("ReadInput")
				{
					NMib::NThread::CMutual InputLock;
					NMib::NStr::CStr Input;
					auto fCreateParams = [&](bool _bExclusive = false)
						{
							auto Params = CStdInReaderParams::fs_Create
								(
									[&](EStdInReaderOutputType _Type, NMib::NStr::CStr const &_Input)
									{
										DMibLock(InputLock);
										Input += _Input;
									}
								)
							;
							Params.m_Flags |= t_bForcePolling ? EStdInReaderFlag_ForcePolling : EStdInReaderFlag_None;
							if (_bExclusive)
								Params.m_Flags |= EStdInReaderFlag_Exclusive;
							return NMib::fg_Move(Params);
						}
					;

					{
						CStdInReader Reader0(fCreateParams());
						CStdInReader Reader1(fCreateParams());
					}
					auto fl_CreateNormal
						= [&]
						{
							CStdInReader Reader1(fCreateParams());
						}
					;
					auto fl_CreateExclusive
						= [&]
						{
							CStdInReader Reader1(fCreateParams(true));
						}
					;
					{
						DMibTestPath("Shared");
						CStdInReader Reader0(fCreateParams());
						DMibTest(DMibExpr(TCThrowsException<NMib::NException::CException>()) == DMibLExpr(fl_CreateExclusive()));
						DMibTest(DMibExpr(TCThrowsException<>()) == DMibLExpr(fl_CreateNormal()));
					}
					{
						DMibTestPath("Exclusive");
						CStdInReader Reader0(fCreateParams(true));
						DMibTest(DMibExpr(TCThrowsException<NMib::NException::CException>()) == DMibLExpr(fl_CreateExclusive()));
						DMibTest(DMibExpr(TCThrowsException<NMib::NException::CException>()) == DMibLExpr(fl_CreateNormal()));
					}
	
				};

				if (fg_TestReportFlags() & ETestReportFlag_ProcessRecursive)
				{
					DMibTestCategory("Process redirection")
					{
						DMibTestSuite("Level2")
						{

							NMib::NThread::CEvent Event;
							NMib::NThread::CMutual InputLock;
							NMib::NStr::CStr Input0;
							CStdInReaderParams Params0
								= CStdInReaderParams::fs_Create
								(
									[&](EStdInReaderOutputType _Type, NMib::NStr::CStr const &_Input)
									{
										DMibLock(InputLock);
										Input0 += _Input;

										if (Input0.f_Find("\r") >= 0 || Input0.f_Find("\n") >= 0)
										{
											Input0 = Input0.f_Trim();
											Event.f_SetSignaled();
										}
									}
								)
							;

							Params0.m_Flags |= t_bForcePolling ? EStdInReaderFlag_ForcePolling : EStdInReaderFlag_None;

							CStdInReader Reader0(NMib::fg_Move(Params0));

							if (Event.f_WaitTimeout(10.0))
								DMibConOut("Timed out waiting for std input. Current input: {}", Input0);
							else
								DMibConOutRaw(Input0);
						};
					};
				}
				else
				{
					DMibTestSuite("Process redirection")
					{
						//--Tests "Malterlib/StdIn/*/Process redirection/Level2" --TestLogger Null --TestResults (All ProcessRecursive)
						//--Tests Malterlib/StdIn* --TestResults (All)
						EExitResult Exited = EExitResult_None;
						uint32 ExitCode = 66;
						NMib::NStr::CStr StdOut;
						NMib::NProcess::CProcessLaunchParams Params = fs_GetLaunchParams("Level2");
						NMib::NProcess::CProcessLaunch *pLauncher = nullptr;

						NMib::NStr::CStr RandomString = NMib::NDataProcessing::fg_GetRandomUuidString() + NMib::NStr::CWStr(str_utf16("日本語"));

						NMib::NPtr::TCUniquePointer<NMib::NThread::CThreadObject> pThreadObject;

						Params.m_fOnStateChange
							= [&](NMib::NProcess::CProcessLaunchStateChangeVariant const &_State, fp64 _TimeSinceStart)
							{
								switch (_State.f_GetTypeID())
								{
								case NMib::NProcess::EProcessLaunchState_Exited:
									{
										NMib::fg_Volatile(Exited) = EExitResult_Exited;
										NMib::fg_Volatile(ExitCode) = _State.f_Get<NMib::NProcess::EProcessLaunchState_Exited>();
									}
									break;
								case NMib::NProcess::EProcessLaunchState_LaunchFailed:
									{
										NMib::fg_Volatile(Exited) = EExitResult_NotLaunched;
										DMibDTrace("Error: {}\r\n", _State.f_Get<NMib::NProcess::EProcessLaunchState_LaunchFailed>());
									}
									break;
								case NMib::NProcess::EProcessLaunchState_Launched:
									{
										pThreadObject 
											= NMib::NThread::CThreadObject::fs_StartThread
											(
												[&](NMib::NThread::CThreadObject *_pThread) -> aint
												{
													NMib::NSys::fg_Thread_Sleep(0.5);
													pLauncher->f_SendStdIn(RandomString);
													pLauncher->f_SendStdIn("\n");

													return 0;
												}
												, "TestThread"
											)
										;
									}
									break;
								}
							}
						;

						Params.m_fOnOutput
							= [&](NMib::NProcess::EProcessLaunchOutputType _OutputType, NMib::NStr::CStr const &_Output)
							{
								if (_OutputType == NMib::NProcess::EProcessLaunchOutputType_StdOut)
								{
									StdOut += _Output;
								}
								else
								{
									DMibDTrace("Unexpected output: {}\r\n", _Output);
								}
							}
						;
						{
							NMib::NProcess::CProcessLaunch Launcher(Params, NMib::NProcess::EProcessLaunchCloseFlag_BlockOnExit);
							pLauncher = &Launcher;
						}



					
						{
							DMibTestPath("Block on exit");
							DMibTest(DMibExpr(Exited) == DMibExpr(EExitResult_Exited));
							DMibTest(DMibExpr(StdOut) == DMibExpr(RandomString));
							DMibTest(DMibExpr(ExitCode) == DMibExpr(0u));
						}
					};
				}
			};
		}

		void f_DoTests()
		{
			f_TestStdIn<false>();
			f_TestStdIn<true>();
		}
	};

	DMibTestRegister(CStdIn_Tests, Malterlib::Process);

}

