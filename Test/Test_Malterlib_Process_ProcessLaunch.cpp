// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Test/Performance>

#include <Mib/Concurrency/ThreadSafeQueue>
#include <Mib/Process/ProxiedProcessLaunch>
#include <Mib/Cryptography/UUID>
#include <Mib/Process/StdIn>


#include "MalterlibBuild.h"

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

	enum EProxyType
	{
		EProxyType_None
		, EProxyType_Proxied
		, EProxyType_ElevatedProxied
	};
	class CProcessLaunch_Tests : public CTest
	{
	public:

		CProcessLaunch_Tests()
		{
		}
		
		zuint8 m_ExitCode;

		static NMib::NStr::CStr fs_GetTestPath(NMib::NStr::CStr const &_TestPath, NMib::NStr::CStr const &_Test, NMib::NStr::CStr const &_Logger = "Registry")
		{
			NMib::NStr::CStr Ret;
			if (_Test.f_IsEmpty())
				Ret = NMib::NStr::CStr::CFormat("--Tests \"{}\" --TestLogger {} --TestResults (All ProcessRecursive)") << _TestPath << _Logger;
			else
				Ret = NMib::NStr::CStr::CFormat("--Tests \"{}/{}\" --TestLogger {} --TestResults (All ProcessRecursive)") << _TestPath << _Test << _Logger;

			auto Groups = NMib::NTest::fg_TestGetCurrentGroups();
			
			if (!Groups.f_IsEmpty())
			{
				Ret += " --TestGroups (";
				for (auto iGroup = Groups.f_GetIterator(); iGroup; ++iGroup)
					Ret += NMib::NStr::CStr::CFormat("{} ") << iGroup.f_GetKey();
				Ret += ")";
			}
			
			return Ret;
		}
		
		NMib::NProcess::CProcessLaunchParams f_GetLaunchParams(NMib::NStr::CStr const &_TestPath = fg_TestGetCurrentPath(), NMib::NStr::CStr const &_Test = "JustExit", NMib::NStr::CStr const &_Logger = "Registry")
		{
			NMib::NProcess::CProcessLaunchParams Params;
			Params.m_Target = NMib::NFile::CFile::fs_GetProgramPath();
			
			if (_Test == "JustExit")
			{
				Params.m_Parameters = NMib::NStr::CStr::CFormat("--StdOutExit {}") << m_ExitCode.f_Get();
			}
			else
				Params.m_Parameters = fs_GetTestPath(_TestPath, _Test, _Logger);
			return Params;
		}
		
		NMib::NThread::CMutual m_ProxyClientLock;
		NMib::NPtr::TCUniquePointer<NMib::NProcess::CProxiedLaunchClient> m_pProxyClient;
		
		zbool m_bProxyElevation;
		zbool m_bFailClient;

		NMib::NProcess::CProxiedLaunchClient &f_GetClient()
		{
			DMibLock(m_ProxyClientLock);
			if (m_pProxyClient)
				return *m_pProxyClient;

			NMib::NProcess::CProcessLaunchParams Params;
			Params.m_Target = NMib::NFile::CFile::fs_GetProgramPath();		
			if (m_bProxyElevation && NMib::NProcess::CProcessLaunch::fs_GetElevation() == NMib::NProcess::EProcessElevation_IsNotElevated)
				Params.m_Elevation = NMib::NProcess::EProcessLaunchElevation_Elevate;

			if (!m_bFailClient)
				Params.m_Parameters = "--Tests Malterlib/Process/ProcessLaunch/ProxyServer --TestLogger Null --TestResults (ProcessRecursive)";
			else
				Params.m_Parameters = "--DoesNotExist";
			
			Params.m_bStdOutPID = 1;
			
			m_pProxyClient = NMib::fg_Construct(Params, NMib::fg_Default());
			
			return *m_pProxyClient;
		}
		
		void f_DoProxyServer()
		{
			if (fg_TestReportFlags() & ETestReportFlag_ProcessRecursive)
			{
				DMibTestSuite("ProxyServer")
				{
					NMib::NProcess::CProxiedLaunchServer Server;
				};
			}
		}
		
		void f_EnableClientFailure(bint _bFail)
		{
			m_bFailClient = _bFail;
			
			DMibLock(m_ProxyClientLock);
			m_pProxyClient = nullptr;
		}
		
		struct CProxiedProcessLaunch
		{
			CProcessLaunch_Tests *m_pThis;
			NMib::NPtr::TCUniquePointer<NMib::NProcess::CVirtualProcessLaunch> m_pLaunch;
			CProxiedProcessLaunch(NMib::NProcess::CProcessLaunchParams const &_Params, NMib::NProcess::EProcessLaunchCloseFlag _Flag, CProcessLaunch_Tests *_pThis)
				: m_pThis(_pThis)
			{
				m_pLaunch = m_pThis->f_GetClient().f_GetFactory()(_Params, _Flag);
			}
			
			void f_Start()
			{
			}
			
			NMib::NProcess::EProcessLaunchCloseFlag f_GetCloseFlags() const
			{
				return m_pLaunch->f_GetCloseFlags();
			}
			
			void f_Close(NMib::NProcess::EProcessLaunchCloseFlag _CloseFlags)
			{
				return m_pLaunch->f_Close(_CloseFlags);
			}
			bint f_IsOpen() const
			{
				return m_pLaunch->f_IsOpen();
			}
			bint f_IsRunning() const
			{
				return m_pLaunch->f_IsRunning();
			}
			void f_SendStdIn(NMib::NStr::CStr const &_Data) const
			{
				return m_pLaunch->f_SendStdIn(_Data);
			}
			fp64 f_GetRunningTime() const
			{
				return m_pLaunch->f_GetRunningTime();
			}			
		};
		
		template <EProxyType t_ProxyType, typename t_CDummy = void>
		struct TCGetProxiedType
		{
			typedef NMib::NProcess::CProcessLaunch CType;
		};

		template <typename t_CDummy>
		struct TCGetProxiedType<EProxyType_Proxied, t_CDummy>
		{
			typedef CProxiedProcessLaunch CType;
		};
		template <typename t_CDummy>
		struct TCGetProxiedType<EProxyType_ElevatedProxied, t_CDummy>
		{
			typedef CProxiedProcessLaunch CType;
		};

		template <bool t_bProxied>
		typename NMib::TCEnableIf<!t_bProxied, NMib::NPtr::TCUniquePointer<NMib::NProcess::CProcessLaunch>>::CType f_CreateLaunch(NMib::NProcess::CProcessLaunchParams const &_Params, NMib::NProcess::EProcessLaunchCloseFlag _Flag)
		{
			return NMib::fg_Construct(_Params, _Flag);
		}

		template <bool t_bProxied>
		typename NMib::TCEnableIf<t_bProxied, NMib::NPtr::TCUniquePointer<CProxiedProcessLaunch>>::CType f_CreateLaunch(NMib::NProcess::CProcessLaunchParams const &_Params, NMib::NProcess::EProcessLaunchCloseFlag _Flag)
		{
			return NMib::fg_Construct(_Params, _Flag, this);
		}
		
		template <EProxyType t_ProxyType>
		void f_TestExecutable(bint _bThreaded)
		{
			DMibTestSuite((_bThreaded ? "Executable" : "Executable non threaded"))
			{
				if (fg_TestReportFlags() & ETestReportFlag_ProcessRecursive)
				{
				}
				else
				{
					EExitResult Exited = EExitResult_None;
					uint32 ExitCode = 66;
					NMib::NStr::CStr StdOut;
					NMib::NProcess::CProcessLaunchParams Params = f_GetLaunchParams();
					Params.m_bThreaded = _bThreaded;
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
						auto pLauncher = f_CreateLaunch<t_ProxyType != EProxyType_None>(Params, NMib::NProcess::EProcessLaunchCloseFlag_BlockOnExit);
						if (!_bThreaded)
							pLauncher->f_Start();
					}
					
					{
						DMibTestPath("Block on exit");
						DMibTest(DMibExpr(Exited) == DMibExpr(EExitResult_Exited));
						DMibTest(DMibExpr(StdOut.f_Find("Footer") >= 0));
						DMibTest(DMibExpr(ExitCode) == DMibExpr(m_ExitCode.f_Get()));
					}

					Exited = EExitResult_None;
					ExitCode = 66;
					StdOut.f_Clear();
					NMib::NAtomic::fg_MemoryFence();

					{
						auto pLauncher = f_CreateLaunch<t_ProxyType != EProxyType_None>(Params, NMib::NProcess::EProcessLaunchCloseFlag_LingerUntilDone);
						if (!_bThreaded)
							pLauncher->f_Start();
					}

					while (NMib::fg_Volatile(Exited) == EExitResult_None)
					{
						NMib::NAtomic::fg_MemoryFence(NMib::NAtomic::EMemoryOrder_Acquire);
						NMib::NSys::fg_Thread_SmallestSleep();
					}
					{
						DMibTestPath("Wait for exit");
						DMibTest(DMibExpr(Exited) == DMibExpr(EExitResult_Exited));
						DMibTest(DMibExpr(StdOut.f_Find("Footer") >= 0));
						DMibTest(DMibExpr(ExitCode) == DMibExpr(m_ExitCode.f_Get()));
					}

					Exited = EExitResult_None;
					ExitCode = 66;
					StdOut.f_Clear();
					NMib::NAtomic::fg_MemoryFence();

					NMib::NContainer::TCThreadSafeQueue<NMib::NFunction::TCFunction<void ()>> DispatchQueue;
					
					Params.m_fDispatcher
						= [&](NMib::NFunction::TCFunction<void ()> const &_ToDispatch)
						{
							DispatchQueue.f_Push(_ToDispatch);
						}
					;


					{
						auto pLauncher = f_CreateLaunch<t_ProxyType != EProxyType_None>(Params, NMib::NProcess::EProcessLaunchCloseFlag_LingerUntilDone);
						if (!_bThreaded)
							pLauncher->f_Start();
					}

					while (NMib::fg_Volatile(Exited) == EExitResult_None)
					{
						NMib::NAtomic::fg_MemoryFence(NMib::NAtomic::EMemoryOrder_Acquire);
						NMib::NSys::fg_Thread_SmallestSleep();
						while (auto QueueEntry = DispatchQueue.f_Pop())
						{
							(*QueueEntry)();
						}
					}
					{
						DMibTestPath("Wait for exit with dispatch");
						DMibTest(DMibExpr(Exited) == DMibExpr(EExitResult_Exited));
						DMibTest(DMibExpr(StdOut.f_Find("Footer") >= 0));
						DMibTest(DMibExpr(ExitCode) == DMibExpr(m_ExitCode.f_Get()));
					}
				}
			};
		}

		template <EProxyType t_ProxyType>
		void f_TestExecutableFromDll()
		{
			DMibTestSuite("Executable from dll")
			{
				if (fg_TestReportFlags() & ETestReportFlag_ProcessRecursive)
				{
				}
				else
				{
					NMib::NStr::CStr DllPath = NMib::NStr::CStr("Test_Malterlib_Helper_Process") + NMib::NFile::CFile::fs_GetDllExtension();
#ifdef DPlatformFamily_Linux
					DllPath = NMib::NFile::CFile::fs_AppendPath(NMib::NFile::CFile::fs_GetProgramDirectory(), DllPath);
#endif
					
					for (mint i = 0; i < 10; ++i)
					{
						void *pDll = NMib::NSys::fg_LoadLibrary(DllPath);

						DMibTest(DMibExpr(pDll))(ETest_FailAndStop)(ETestFlag_Aggregated);
						
						uint32 (calling_convention_c *fLaunchInDll)(ch8 const *_pProgram, ch8 const *_pParams) = nullptr;

						(void * &)fLaunchInDll = NMib::NSys::fg_GetLibrarySymbol(pDll, "fg_Launch");
						DMibTest(DMibExpr(fLaunchInDll))(ETest_FailAndStop)(ETestFlag_Aggregated);
						
						NMib::NProcess::CProcessLaunchParams Params = f_GetLaunchParams();
						
						[[maybe_unused]] uint32 LaunchResult = fLaunchInDll(Params.m_Target, Params.m_Parameters);
						
						NMib::NSys::fg_FreeLibrary(pDll);
					}
				}
			};
		}
		
		template <EProxyType t_ProxyType>
		void f_TestPerformance(bool _bThreaded)
		{
			DMibTestSuite(CTestCategory(_bThreaded ? "Performance" : "Performance non threaded") << CTestGroup("Performance"))
			{
				EExitResult Exited = EExitResult_None;
				uint32 ExitCode = 66;
				NMib::NStr::CStr StdOut;
				NMib::NProcess::CProcessLaunchParams Params;
				Params.m_Target = NMib::NFile::CFile::fs_GetProgramPath();
				Params.m_Parameters = NMib::NStr::CStr::CFormat("--JustExit {}") << m_ExitCode.f_Get();
				Params.m_bThreaded = _bThreaded;

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
				CTestPerformanceMeasure MalterlibTime("Malterlib");

				mint nTests = 256 + 1;
				mint nLoops = 1;

				for(mint j = 0; j < nTests; ++j)
				{
					MalterlibTime.f_Start();
					for (mint i = 0; i < nLoops; ++i) 
					{
						Exited = EExitResult_None;
						ExitCode = 66;
						StdOut.f_Clear();
						NMib::NAtomic::fg_MemoryFence();
						{
							auto pLauncher = f_CreateLaunch<t_ProxyType != EProxyType_None>(Params, NMib::NProcess::EProcessLaunchCloseFlag_BlockOnExit);
							if (!_bThreaded)
								pLauncher->f_Start();
						}
					
						{
							DMibTest(DMibExpr(Exited) == DMibExpr(EExitResult_Exited))(ETestFlag_Aggregated);
							DMibTest(DMibExpr(ExitCode) == DMibExpr(m_ExitCode.f_Get()))(ETestFlag_Aggregated);
						}
					}
					MalterlibTime.f_Stop(nLoops);
				}

				CTestPerformance PerfTest(1.0);
				PerfTest.f_Add(MalterlibTime);
				DMibTest(DMibExpr(PerfTest));

			};
		}

		template <EProxyType t_ProxyType>
		void f_TestDocument()
		{
/*
#ifdef DPlatformFamily_OSX
			return;
#endif
*/
			DMibTestSuite("Document")
			{
				if (fg_TestReportFlags() & ETestReportFlag_ProcessRecursive)
				{
					NMib::NSys::fg_Thread_Sleep(0.01f);
					NMib::NTest::fg_TestSetReturnValue(m_ExitCode.f_Get());

					NMib::NStr::CStr ExtraData = fg_TestGetExtraData();
					aint iProtocol = ExtraData.f_Find("://");
					if (iProtocol >= 0)
						ExtraData = ExtraData.f_Extract(iProtocol + 3);

					NMib::NFile::CFile::fs_CreateDirectory(NMib::NFile::CFile::fs_GetPath(ExtraData));
					NMib::NFile::CFile File;
					File.f_Open(ExtraData, NMib::NFile::EFileOpen_Write | NMib::NFile::EFileOpen_ShareAll);
				}
				else
				{
#ifdef DPlatformFamily_Windows
					NMib::NStr::CStr URLTest = NMib::NDataProcessing::fg_GetHashedUuidString(fg_TestGetCurrentPath(), NMib::NDataProcessing::CUniversallyUniqueIdentifier("{3CD79DA2-0245-4662-A4E2-153B32B8FCE2}"));
#else
					NMib::NStr::CStr URLTest;
#endif
					NMib::NStr::CStr URLHandler = DLaunchableScheme + URLTest; // NMib::NStr::CStr::CFormat("idslaunch{}") << NMib::NMisc::g_Random->f_GetValue<uint32>();

					#ifdef DPlatformFamily_Windows
						NMib::NProcess::CProcessLaunch::fs_RegisterURLHandler(URLHandler, NMib::NFile::CFile::fs_GetProgramPath(), NMib::NStr::CStr::CFormat("--Tests \"{}\" --TestLogger Registry --TestResults (All ProcessRecursive) --TestData \"%1\"") << fg_TestGetCurrentPath());
						auto Cleanup
							= NMib::fg_OnScopeExit
							(
								[&]()
								{
									NMib::NProcess::CProcessLaunch::fs_DeRegisterURLHandler(URLHandler);
								}
							)
						;
					#endif

					{
						NMib::NStr::CStr TestFilePath = NMib::NFile::CFile::fs_GetProgramDirectory() + "/" DLaunchableScheme + URLTest + "_Handled.txt";
						if (NMib::NFile::CFile::fs_FileExists(TestFilePath))
						{
							NMib::NFile::CFile::fs_DeleteFile(TestFilePath);
						}

						EExitResult Exited = EExitResult_None;
						uint32 ExitCode = 66;
						NMib::NProcess::CProcessLaunchParams Params;
						NMib::NStr::CStr LaunchError;

						Params.m_LaunchType = NMib::NProcess::EProcessLaunchType_Document;
						Params.m_Target = URLHandler + "://" + TestFilePath;
						Params.m_bShowLaunched = false;
						Params.m_fOnStateChange
							= [&](NMib::NProcess::CProcessLaunchStateChangeVariant const &_State, fp64 _TimeSinceStart)
							{
								switch (_State.f_GetTypeID())
								{
								case NMib::NProcess::EProcessLaunchState_Launched:
									{
										void* pProcess = _State.f_Get<NMib::NProcess::EProcessLaunchState_Launched>();
										if (!pProcess)
										{
											if (NMib::fg_Volatile(ExitCode) == 66)
												NMib::fg_Volatile(ExitCode) = m_ExitCode.f_Get();
										}
									}
									break;
								case NMib::NProcess::EProcessLaunchState_Exited:
									{
										NMib::fg_Volatile(Exited) = EExitResult_Exited;
										NMib::fg_Volatile(ExitCode) = _State.f_Get<NMib::NProcess::EProcessLaunchState_Exited>();
									}
									break;
								case NMib::NProcess::EProcessLaunchState_LaunchFailed:
									{
										NMib::fg_Volatile(Exited) = EExitResult_NotLaunched;
										LaunchError = _State.f_Get<NMib::NProcess::EProcessLaunchState_LaunchFailed>();
									}
									break;
								}
							}
						;

						{
							auto pLauncher = f_CreateLaunch<t_ProxyType != EProxyType_None>(Params, NMib::NProcess::EProcessLaunchCloseFlag_BlockOnExit);
						}
					
						{
							DMibTestPath("Block on exit");
							DMibTest(DMibExpr(Exited) == DMibExpr(EExitResult_Exited));
							DMibTest(DMibExpr(LaunchError) == DMibExpr(""));
							DMibTest(DMibExpr(ExitCode) == DMibExpr(m_ExitCode.f_Get()));
							DMibTest(DMibExpr(NMib::NFile::CFile::fs_FileExists(TestFilePath)));
						}

						if (NMib::NFile::CFile::fs_FileExists(TestFilePath))
						{
							NMib::NFile::CFile::fs_DeleteFile(TestFilePath);
						}

						Exited = EExitResult_None;
						ExitCode = 66;
						LaunchError.f_Clear();
						NMib::NAtomic::fg_MemoryFence();

						{
							auto pLauncher = f_CreateLaunch<t_ProxyType != EProxyType_None>(Params, NMib::NProcess::EProcessLaunchCloseFlag_LingerUntilDone);
						}

						while (NMib::fg_Volatile(Exited) == EExitResult_None)
						{
							NMib::NAtomic::fg_MemoryFence(NMib::NAtomic::EMemoryOrder_Acquire);
							NMib::NSys::fg_Thread_SmallestSleep();
						}
						{
							DMibTestPath("Wait for exit");
							DMibTest(DMibExpr(Exited) == DMibExpr(EExitResult_Exited));
							DMibTest(DMibExpr(LaunchError) == DMibExpr(""));
							DMibTest(DMibExpr(ExitCode) == DMibExpr(m_ExitCode.f_Get()));
							DMibTest(DMibExpr(NMib::NFile::CFile::fs_FileExists(TestFilePath)));
						}

						Exited = EExitResult_None;
						ExitCode = 66;
						LaunchError.f_Clear();
						NMib::NAtomic::fg_MemoryFence();

						NMib::NContainer::TCThreadSafeQueue<NMib::NFunction::TCFunction<void ()>> DispatchQueue;
					
						Params.m_fDispatcher
							= [&](NMib::NFunction::TCFunction<void ()> const &_ToDispatch)
							{
								DispatchQueue.f_Push(_ToDispatch);
							}
						;

						if (NMib::NFile::CFile::fs_FileExists(TestFilePath))
						{
							NMib::NFile::CFile::fs_DeleteFile(TestFilePath);
						}

						{
							auto pLauncher = f_CreateLaunch<t_ProxyType != EProxyType_None>(Params, NMib::NProcess::EProcessLaunchCloseFlag_LingerUntilDone);
						}

						while (NMib::fg_Volatile(Exited) == EExitResult_None)
						{
							NMib::NAtomic::fg_MemoryFence(NMib::NAtomic::EMemoryOrder_Acquire);
							NMib::NSys::fg_Thread_SmallestSleep();
							while (auto QueueEntry = DispatchQueue.f_Pop())
							{
								(*QueueEntry)();
							}
						}
						{
							DMibTestPath("Wait for exit with dispatch");
							DMibTest(DMibExpr(Exited) == DMibExpr(EExitResult_Exited));
							DMibTest(DMibExpr(LaunchError) == DMibExpr(""));
							DMibTest(DMibExpr(ExitCode) == DMibExpr(m_ExitCode.f_Get()));
							DMibTest(DMibExpr(NMib::NFile::CFile::fs_FileExists(TestFilePath)));
						}
					}
				}
			};
		}
		
		void f_TestElevationStdIn()
		{
			
			DMibTestSuite(NMib::NTest::CTestCategory("ElevationStdIn") << NMib::NTest::CTestGroup("Manual"))
			{
				// This has to be enabled manually as you need to press the button to allow elevation
				if (fg_TestReportFlags() & ETestReportFlag_ProcessRecursive)
				{
					if (NMib::NProcess::CProcessLaunch::fs_GetElevation() != NMib::NProcess::EProcessElevation_IsElevated &&
						NMib::NProcess::CProcessLaunch::fs_GetElevation() != NMib::NProcess::EProcessElevation_IsRoot)
					{
						DMibConErrOutRaw("Not elevated");
					}
					
					DMibConErrOutRaw("Test stderr");
					NMib::NTest::fg_TestSetReturnValue(m_ExitCode.f_Get());
					
					NMib::NStr::CStr Input;
					NMib::NThread::CEvent Event;
					
					NMib::NProcess::CStdInReaderParams Params
						= NMib::NProcess::CStdInReaderParams::fs_Create
						(
							[&](NMib::NProcess::EStdInReaderOutputType _Type, NMib::NStr::CStr const &_Input)
							{
								Input += _Input;
								
								if (_Input.f_FindChar('\n') > -1)
								{
									Event.f_SetSignaled();
								}
							}
						)
					;

					NMib::NProcess::CStdInReader Reader(NMib::fg_Move(Params));
					
					if (Event.f_WaitTimeout(10.0))
						DMibConOut("Timed out waiting for std input. Current input: {}", Input);
					else
						DMibConOutRaw(Input);
					
				}
				else
				{
					if (NMib::NProcess::CProcessLaunch::fs_GetElevation() == NMib::NProcess::EProcessElevation_IsNotElevated)
					{
						NMib::NProcess::CProcessLaunch *pLauncher = nullptr;
						NMib::NStr::CStr RandomString = NMib::NDataProcessing::fg_GetRandomUuidString() + NMib::NStr::CWStr(str_utf16("日本語"));
						
						EExitResult Exited = EExitResult_None;
						uint32 ExitCode = 66;
						NMib::NProcess::CProcessLaunchParams Params = f_GetLaunchParams(fg_TestGetCurrentPath(), "", "Null");
						Params.m_bShowLaunched = false;
						Params.m_Elevation = NMib::NProcess::EProcessLaunchElevation_Elevate;
						Params.m_bStdOutPID = true;
						Params.m_Prompt = "You need to elevate to run the elevation tests.\"This is quoted\"\nSecond line";
														
						Params.m_fOnStateChange
							= [&](NMib::NProcess::CProcessLaunchStateChangeVariant const &_State, fp64 _TimeSinceStart)
							{						
								switch (_State.f_GetTypeID())
								{
								case NMib::NProcess::EProcessLaunchState_Launched:
									{
										pLauncher->f_SendStdIn(RandomString);
										pLauncher->f_SendStdIn("\n");
									}
									break;
								case NMib::NProcess::EProcessLaunchState_Exited:
									{
										NMib::fg_Volatile(Exited) = EExitResult_Exited;
										NMib::fg_Volatile(ExitCode) = _State.f_Get<NMib::NProcess::EProcessLaunchState_Exited>();
									}
									break;
								case NMib::NProcess::EProcessLaunchState_LaunchFailed:
									{
										NMib::fg_Volatile(Exited) = EExitResult_NotLaunched;
										DMibConOut("Error: {}\r\n", _State.f_Get<NMib::NProcess::EProcessLaunchState_LaunchFailed>());
									}
									break;
								}
								
							}
						;
						
						NMib::NStr::CStr StdErr;
						NMib::NStr::CStr StdOut;
						
						Params.m_fOnOutput
							= [&StdErr, &StdOut](NMib::NProcess::EProcessLaunchOutputType _OutputType, NMib::NStr::CStr const &_Output)
							{							
								if (_OutputType == NMib::NProcess::EProcessLaunchOutputType_StdErr)
								{
									StdErr += _Output;
								}
								else if (_OutputType == NMib::NProcess::EProcessLaunchOutputType_StdOut)
								{
									StdOut += _Output;
								}
							}
						;
						
						{
							NMib::NProcess::CProcessLaunch Launcher(Params, NMib::NProcess::EProcessLaunchCloseFlag_BlockOnExit);
							pLauncher = &Launcher;
						}
						
						StdOut = StdOut.f_Trim();
					
						{
							DMibTestPath("Block on exit");
							DMibTest(DMibExpr(Exited) == DMibExpr(EExitResult_Exited));
#ifndef DPlatformFamily_Linux // Buggy gksu does not forward exit code
							DMibTest(DMibExpr(ExitCode) == DMibExpr(m_ExitCode.f_Get()));
#endif
							DMibTest(DMibExpr(StdErr) == DMibExpr("Test stderr"));
							DMibTest(DMibExpr(StdOut) == DMibExpr(RandomString));
						}
					}
				}
			};
		}

		template <EProxyType t_ProxyType>
		void f_TestElevation()
		{
			DMibTestSuite(NMib::NTest::CTestCategory("Elevation") << NMib::NTest::CTestGroup("Manual"))
			{
				// This has to be enabled manually as you need to press the button to allow elevation
				if (fg_TestReportFlags() & ETestReportFlag_ProcessRecursive)
				{
					if (NMib::NProcess::CProcessLaunch::fs_GetElevation() != NMib::NProcess::EProcessElevation_IsElevated &&
						NMib::NProcess::CProcessLaunch::fs_GetElevation() != NMib::NProcess::EProcessElevation_IsRoot)
					{
						DMibConErrOutRaw("Not elevated");
					}
					
					DMibConOutRaw("Test stdout");
					DMibConErrOutRaw("Test stderr");
					NMib::NTest::fg_TestSetReturnValue(m_ExitCode.f_Get());
					
				}
				else
				{
#ifdef DPlatformFamily_Linux
					if (NMib::NProcess::CProcessLaunch::fs_GetElevation() == NMib::NProcess::EProcessElevation_IsNotElevated && t_ProxyType != EProxyType_ElevatedProxied)
#else
					if (NMib::NProcess::CProcessLaunch::fs_GetElevation() == NMib::NProcess::EProcessElevation_IsNotElevated)
#endif
					{
						EExitResult Exited = EExitResult_None;
						uint32 ExitCode = 66;
						NMib::NProcess::CProcessLaunchParams Params = f_GetLaunchParams(fg_TestGetCurrentPath(), "", "Null");
						Params.m_bShowLaunched = false;
						Params.m_Elevation = NMib::NProcess::EProcessLaunchElevation_Elevate;
						Params.m_bStdOutPID = true;
						Params.m_Prompt = "You need to elevate to run the elevation tests.\"This is quoted\"\nSecond line";
														
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
										//DMibTrace("Launch failed error: {}\r\n", _State.f_Get<NMib::NProcess::EProcessLaunchState_LaunchFailed>());
									}
									break;
								}
								
							}
						;
						
						NMib::NStr::CStr StdErr;
						NMib::NStr::CStr StdOut;
						
						Params.m_fOnOutput
							= [&StdErr, &StdOut](NMib::NProcess::EProcessLaunchOutputType _OutputType, NMib::NStr::CStr const &_Output)
							{							
								if (_OutputType == NMib::NProcess::EProcessLaunchOutputType_StdErr)
								{
									StdErr += _Output;
								}
								else if (_OutputType == NMib::NProcess::EProcessLaunchOutputType_StdOut)
								{
									StdOut += _Output;
								}
							}
						;
						
						{
							auto pLauncher = f_CreateLaunch<t_ProxyType != EProxyType_None>(Params, NMib::NProcess::EProcessLaunchCloseFlag_BlockOnExit);
						}
					
						{
							DMibTestPath("Block on exit");
							DMibTest(DMibExpr(Exited) == DMibExpr(EExitResult_Exited));
#ifndef DPlatformFamily_Linux // Buggy gksu does not forward exit code
							DMibTest(DMibExpr(ExitCode) == DMibExpr(m_ExitCode.f_Get()));
#endif
							DMibTest(DMibExpr(StdErr) == DMibExpr("Test stderr"));
							DMibTest(DMibExpr(StdOut) == DMibExpr("Test stdout"));
						}
						
						StdErr = NMib::NStr::CStr();
						StdOut = NMib::NStr::CStr();
						Exited = EExitResult_None;
						ExitCode = 66;
						NMib::NStr::CStr OldTarget = Params.m_Target;
						Params.m_Target = "/Blah";
						NMib::NAtomic::fg_MemoryFence();
											
						{
							auto pLauncher = f_CreateLaunch<t_ProxyType != EProxyType_None>(Params, NMib::NProcess::EProcessLaunchCloseFlag_BlockOnExit);
						}
						
						{
							DMibTestPath("Block on exit");
							DMibTest(DMibExpr(Exited) == DMibExpr(EExitResult_NotLaunched));
							DMibTest(DMibExpr(ExitCode) == DMibExpr(66));
							DMibTest(DMibExpr(StdErr) == DMibExpr(""));
							DMibTest(DMibExpr(StdOut) == DMibExpr(""));
						}

						StdErr = NMib::NStr::CStr();
						StdOut = NMib::NStr::CStr();
						Exited = EExitResult_None;
						ExitCode = 66;
						Params.m_Target = OldTarget;
						NMib::NAtomic::fg_MemoryFence();

						{
							auto pLauncher = f_CreateLaunch<t_ProxyType != EProxyType_None>(Params, NMib::NProcess::EProcessLaunchCloseFlag_LingerUntilDone);
						}

						while (NMib::fg_Volatile(Exited) == EExitResult_None)
						{
							NMib::NAtomic::fg_MemoryFence(NMib::NAtomic::EMemoryOrder_Acquire);
							NMib::NSys::fg_Thread_SmallestSleep();
						}
						{
							DMibTestPath("Wait for exit");
							DMibTest(DMibExpr(Exited) == DMibExpr(EExitResult_Exited));
#ifndef DPlatformFamily_Linux // Buggy gksu does not forward exit code
							DMibTest(DMibExpr(ExitCode) == DMibExpr(m_ExitCode.f_Get()));
#endif
							DMibTest(DMibExpr(StdErr) == DMibExpr("Test stderr"));
							DMibTest(DMibExpr(StdOut) == DMibExpr("Test stdout"));
						}

						StdErr = NMib::NStr::CStr();
						StdOut = NMib::NStr::CStr();
						Exited = EExitResult_None;
						ExitCode = 66;
						NMib::NAtomic::fg_MemoryFence();

						NMib::NContainer::TCThreadSafeQueue<NMib::NFunction::TCFunction<void ()>> DispatchQueue;
					
						Params.m_fDispatcher
							= [&](NMib::NFunction::TCFunction<void ()> const &_ToDispatch)
							{
								DispatchQueue.f_Push(_ToDispatch);
							}
						;


						{
							auto pLauncher = f_CreateLaunch<t_ProxyType != EProxyType_None>(Params, NMib::NProcess::EProcessLaunchCloseFlag_LingerUntilDone);
						}

						while (NMib::fg_Volatile(Exited) == EExitResult_None)
						{
							NMib::NAtomic::fg_MemoryFence(NMib::NAtomic::EMemoryOrder_Acquire);
							NMib::NSys::fg_Thread_SmallestSleep();
							while (auto QueueEntry = DispatchQueue.f_Pop())
							{
								(*QueueEntry)();
							}
						}
						{
							DMibTestPath("Wait for exit with dispatch");
							DMibTest(DMibExpr(Exited) == DMibExpr(EExitResult_Exited));
#ifndef DPlatformFamily_Linux // Buggy gksu does not forward exit code
							DMibTest(DMibExpr(ExitCode) == DMibExpr(m_ExitCode.f_Get()));
#endif
							DMibTest(DMibExpr(StdErr) == DMibExpr("Test stderr"));
							DMibTest(DMibExpr(StdOut) == DMibExpr("Test stdout"));
						}
						
						
					}
				}
			};
		}

		template <EProxyType t_ProxyType>
		void f_TestDeElevation()
		{
#ifndef DPlatformFamily_Windows
			return;
#endif
			DMibTestSuite(NMib::NTest::CTestCategory("De-elevation") << NMib::NTest::CTestGroup("Manual"))
			{
				// This has to be enabled manually as you need to press the button to allow elevation
				if (fg_TestReportFlags() & ETestReportFlag_ProcessRecursive)
				{
					NMib::NStr::CStr TestPath = fg_TestGetCurrentPath();

					EExitResult Exited = EExitResult_None;
					uint32 ExitCode = 66;
					NMib::NProcess::CProcessLaunchParams Params = f_GetLaunchParams(TestPath);
					Params.m_bShowLaunched = false;
					Params.m_Elevation = NMib::NProcess::EProcessLaunchElevation_DeElevate;
					Params.m_Parameters = fs_GetTestPath(TestPath, "CheckElevation");
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
							}
						}
					;

					DMibTestSuite("CheckElevation")
					{
						DMibTest(DMibExpr(NMib::NProcess::CProcessLaunch::fs_GetElevation()) == DMibExpr(NMib::NProcess::EProcessElevation_IsNotElevated));
					};

					DMibTestSuite("Block on exit")
					{
						{
							auto pLauncher = f_CreateLaunch<t_ProxyType != EProxyType_None>(Params, NMib::NProcess::EProcessLaunchCloseFlag_BlockOnExit);
						}
						DMibTest(DMibExpr(ExitCode) == DMibExpr(m_ExitCode.f_Get()));
						DMibTest(DMibExpr(Exited) == DMibExpr(EExitResult_Exited));
					};
					

					DMibTestSuite("Wait for exit")
					{
						Exited = EExitResult_None;
						ExitCode = 66;
						NMib::NAtomic::fg_MemoryFence();

						{
							auto pLauncher = f_CreateLaunch<t_ProxyType != EProxyType_None>(Params, NMib::NProcess::EProcessLaunchCloseFlag_LingerUntilDone);
						}

						while (NMib::fg_Volatile(Exited) == EExitResult_None)
						{
							NMib::NAtomic::fg_MemoryFence(NMib::NAtomic::EMemoryOrder_Acquire);
							NMib::NSys::fg_Thread_SmallestSleep();
						}
						DMibTest(DMibExpr(ExitCode) == DMibExpr(m_ExitCode.f_Get()));
						DMibTest(DMibExpr(Exited) == DMibExpr(EExitResult_Exited));
					};

					DMibTestSuite("Wait for exit with dispatch")
					{
						Exited = EExitResult_None;
						ExitCode = 66;
						NMib::NAtomic::fg_MemoryFence();

						NMib::NContainer::TCThreadSafeQueue<NMib::NFunction::TCFunction<void ()>> DispatchQueue;
					
						Params.m_fDispatcher
							= [&](NMib::NFunction::TCFunction<void ()> const &_ToDispatch)
							{
								DispatchQueue.f_Push(_ToDispatch);
							}
						;


						{
							auto pLauncher = f_CreateLaunch<t_ProxyType != EProxyType_None>(Params, NMib::NProcess::EProcessLaunchCloseFlag_LingerUntilDone);
						}

						while (NMib::fg_Volatile(Exited) == EExitResult_None)
						{
							NMib::NAtomic::fg_MemoryFence(NMib::NAtomic::EMemoryOrder_Acquire);
							NMib::NSys::fg_Thread_SmallestSleep();
							while (auto QueueEntry = DispatchQueue.f_Pop())
							{
								(*QueueEntry)();
							}
						}
						DMibTest(DMibExpr(ExitCode) == DMibExpr(m_ExitCode.f_Get()));
						DMibTest(DMibExpr(Exited) == DMibExpr(EExitResult_Exited));
					};
				}
				else
				{
					NMib::NStr::CStr TestPath = fg_TestGetCurrentPath();
					DMibTestSuite("RunTest")
					{
						if (NMib::NProcess::CProcessLaunch::fs_GetElevation() == NMib::NProcess::EProcessElevation_IsNotElevated)
						{
							EExitResult Exited = EExitResult_None;
							uint32 ExitCode = 66;
							NMib::NProcess::CProcessLaunchParams Params = CProcessLaunch_Tests::f_GetLaunchParams(TestPath);
							Params.m_bShowLaunched = false;
							Params.m_Elevation = NMib::NProcess::EProcessLaunchElevation_Elevate;

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
									}
								}
							;

							{
								Params.m_Parameters = CProcessLaunch_Tests::fs_GetTestPath(TestPath, "Block on exit");
								auto pLauncher = f_CreateLaunch<t_ProxyType != EProxyType_None>(Params, NMib::NProcess::EProcessLaunchCloseFlag_BlockOnExit);
							}
					
							{
								DMibTestPath("Block on exit");
								DMibTest(DMibExpr(Exited) == DMibExpr(EExitResult_Exited));
								DMibTest(DMibExpr(ExitCode) == DMibExpr(m_ExitCode.f_Get()));
							}

							Exited = EExitResult_None;
							ExitCode = 66;
							NMib::NAtomic::fg_MemoryFence();

							{
								Params.m_Parameters = CProcessLaunch_Tests::fs_GetTestPath(TestPath, "Wait for exit");
								auto pLauncher = f_CreateLaunch<t_ProxyType != EProxyType_None>(Params, NMib::NProcess::EProcessLaunchCloseFlag_LingerUntilDone);
							}

							while (NMib::fg_Volatile(Exited) == EExitResult_None)
							{
								NMib::NAtomic::fg_MemoryFence(NMib::NAtomic::EMemoryOrder_Acquire);
								NMib::NSys::fg_Thread_SmallestSleep();
							}
							{
								DMibTestPath("Wait for exit");
								DMibTest(DMibExpr(Exited) == DMibExpr(EExitResult_Exited));
								DMibTest(DMibExpr(ExitCode) == DMibExpr(m_ExitCode.f_Get()));
							}

							Exited = EExitResult_None;
							ExitCode = 66;
							NMib::NAtomic::fg_MemoryFence();

							NMib::NContainer::TCThreadSafeQueue<NMib::NFunction::TCFunction<void ()>> DispatchQueue;
					
							Params.m_fDispatcher
								= [&](NMib::NFunction::TCFunction<void ()> const &_ToDispatch)
								{
									DispatchQueue.f_Push(_ToDispatch);
								}
							;


							{
								Params.m_Parameters = CProcessLaunch_Tests::fs_GetTestPath(TestPath, "Wait for exit with dispatch");
								auto pLauncher = f_CreateLaunch<t_ProxyType != EProxyType_None>(Params, NMib::NProcess::EProcessLaunchCloseFlag_LingerUntilDone);
							}

							while (NMib::fg_Volatile(Exited) == EExitResult_None)
							{
								NMib::NAtomic::fg_MemoryFence(NMib::NAtomic::EMemoryOrder_Acquire);
								NMib::NSys::fg_Thread_SmallestSleep();
								while (auto QueueEntry = DispatchQueue.f_Pop())
								{
									(*QueueEntry)();
								}
							}
							{
								DMibTestPath("Wait for exit with dispatch");
								DMibTest(DMibExpr(Exited) == DMibExpr(EExitResult_Exited));
								DMibTest(DMibExpr(ExitCode) == DMibExpr(m_ExitCode.f_Get()));
							}
						}
					};
				}
			};
		}

		template <EProxyType t_ProxyType>
		void f_TestSandbox()
		{
			DMibTestSuite("Sandbox")
			{
#ifdef DPlatformFamily_Windows
				NMib::NStr::CStr Sandbox = "X:";
#else
				NMib::NStr::CStr Sandbox = "/";
#endif
				if (fg_TestReportFlags() & ETestReportFlag_ProcessRecursive)
				{
#ifdef DPlatformFamily_Windows
					NMib::NStr::CStr File = NMib::NStr::CStr("X:/TestFile.txt");
#else
					NMib::NStr::CStr File = NMib::NStr::CStr("/TestFile.txt");
#endif
					bool bExists = NMib::NFile::CFile::fs_FileExists(File);
					DMibTest(DMibExpr(bExists));
					NMib::NTest::fg_TestSetReturnValue(m_ExitCode.f_Get());
				}
				else
				{
					{
						EExitResult Exited = EExitResult_None;
						uint32 ExitCode = 66;
						NMib::NStr::CStr StdOut;
						NMib::NProcess::CProcessLaunchParams Params = f_GetLaunchParams();

						Params.m_Parameters = NMib::NStr::CStr::CFormat("--Tests \"{}\" --TestLogger Registry --TestResults (All ProcessRecursive)") << fg_TestGetCurrentPath();

						NMib::NStr::CStr SandboxPath = NMib::NFile::CFile::fs_GetProgramDirectory() + "/SandboxTest";
						NMib::NStr::CStr SandboxFile = SandboxPath + "/TestFile.txt";
						NMib::NFile::CFile::fs_CreateDirectory(SandboxPath);
						{
							NMib::NFile::CFile File;
							File.f_Open(SandboxFile, NMib::NFile::EFileOpen_Write);
						}

						Params.m_bSandboxed = true;
#ifdef DPlatformFamily_Windows
						Params.m_SandboxRoots["X:"] = SandboxPath;
#else
						Params.m_SandboxRoots[NMib::NStr::CStr()] = SandboxPath;
#endif

						//Params.
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
								}
							}
						;

						Params.m_fOnOutput
							= [&](NMib::NProcess::EProcessLaunchOutputType _OutputType, NMib::NStr::CStr const &_Output)
							{
								if (_OutputType == NMib::NProcess::EProcessLaunchOutputType_StdOut)
									StdOut += _Output;
								else
									DMibDTrace("Unexpected output: {}\r\n", _Output);
							}
						;

						{
							auto pLauncher = f_CreateLaunch<t_ProxyType != EProxyType_None>(Params, NMib::NProcess::EProcessLaunchCloseFlag_BlockOnExit);
						}

						{
							DMibTestPath("Block on exit");
							DMibTest(DMibExpr(Exited) == DMibExpr(EExitResult_Exited));
							DMibTest(DMibExpr(ExitCode) == DMibExpr(m_ExitCode.f_Get()));
						}
					}
				}
			};
		}
		
		template <EProxyType t_ProxyType>
		void f_TestLimits()
		{
			if (fg_TestReportFlags() & ETestReportFlag_ProcessRecursive)
			{
				DMibTestSuite("Limits")
				{
					try
					{
						NMib::NSys::fg_Thread_SetPriority(NMib::NSys::fg_Thread_GetCurrent(), NMib::EThreadPriority_Lowest);
					}
					catch (NMib::NException::CException const &)
					{
					}
					
					NMib::NTime::CClock Clock;
					Clock.f_Start();
					// Block for 1 seconds
					while (Clock.f_GetTime() < 1.0)
						;
					
					NMib::NTest::fg_TestSetReturnValue(m_ExitCode.f_Get());
				};
			}
			else
			{
				DMibTestSuite("Limits")
				{
					EExitResult Exited = EExitResult_None;
					uint32 ExitCode = 66;
					NMib::NStr::CStr StdOut;
					NMib::NProcess::CProcessLaunchParams Params = f_GetLaunchParams(fg_TestGetCurrentPath(), "");
					Params.m_bThreaded = true;
					NMib::NThread::CMutual Lock;
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
							}
						}
					;
					
					mint nCores = NMib::NSys::fg_Thread_GetVirtualCores();
					Params.m_LaunchPriority = NMib::EExecutionPriority_Lowest;
					Params.m_CPUUsage = NMib::fg_Max(fp64(nCores) - 2.0, fp64(1.0)) / fp64(nCores);
					//Params.m_CPUUsage = fp64(1.0) / fp64(nCores);
					//Params.m_CPUUsage = 0.5;
					Params.m_ProcessGroup = "LimitsTest";

					Params.m_fOnOutput
						= [&](NMib::NProcess::EProcessLaunchOutputType _OutputType, NMib::NStr::CStr const &_Output)
						{
							if (_OutputType == NMib::NProcess::EProcessLaunchOutputType_StdOut)
							{
								DMibLock(Lock);
								StdOut += _Output;
							}
							else
							{
								DMibDTrace("Unexpected output: {}\r\n", _Output);
							}
						}
					;
					{
						mint nLaunches = NMib::fg_Min(nCores*16, 64u);
						NMib::NContainer::TCVector<NMib::NPtr::TCUniquePointer<typename TCGetProxiedType<t_ProxyType>::CType>> Launches;
						for (mint i = 0; i < nLaunches; ++i)
							Launches.f_Insert(f_CreateLaunch<t_ProxyType != EProxyType_None>(Params, NMib::NProcess::EProcessLaunchCloseFlag_BlockOnExit));
					}
					
					{
						DMibTestPath("Block on exit");
						DMibTest(DMibExpr(Exited) == DMibExpr(EExitResult_Exited));
						DMibTest(DMibExpr(StdOut.f_Find("Footer") >= 0));
						DMibTest(DMibExpr(ExitCode) == DMibExpr(m_ExitCode.f_Get()));
					}
				};
			}
		}

		NMib::NStr::CStr fp_GetLongOutput()
		{
			NMib::NStr::CStr Ret;
			
			NMib::NMisc::CRandomShiftRNG Random;
			for (auto i = 0; i < 1024*1024; ++i)
			{
				if (i % 1024 == 0 && i != 0)
					Ret += "\n";
				Ret += NMib::NStr::CStr::CFormat("{nfh,sj1,sf0}") << Random.f_GetValue<uint32>();
					
			}
			
			return Ret;
		}
		template <EProxyType t_ProxyType>
		void f_TestStdOut()
		{
			DMibTestSuite("StdOut")
			{
				if (fg_TestReportFlags() & ETestReportFlag_ProcessRecursive)
				{
					DMibConErrOutRaw(fp_GetLongOutput());
					NMib::NTest::fg_TestSetReturnValue(m_ExitCode.f_Get());
					
/*					NMib::NMisc::CRandomShiftRNG Random;
					for (auto i = 0; i < 256; ++i)
						DMibConOut("{nfh,sj*,sf0}", Random.f_GetValue<uint32>() << i);
					
					DMibConOutRaw("\n");*/
					
				}
				else
				{
					EExitResult Exited = EExitResult_None;
					uint32 ExitCode = 66;
					NMib::NStr::CStr StdOut;
					NMib::NProcess::CProcessLaunchParams Params = f_GetLaunchParams(fg_TestGetCurrentPath(), "");
					Params.m_Parameters = fs_GetTestPath(fg_TestGetCurrentPath(), "", "Null");
					
					Params.m_bThreaded = true;
					NMib::NThread::CMutual Lock;
					Params.m_fOnStateChange
						= [&](NMib::NProcess::CProcessLaunchStateChangeVariant const &_State, fp64 _TimeSinceStart)
						{
							switch (_State.f_GetTypeID())
							{
								case NMib::NProcess::EProcessLaunchState_Launched:
								{
								}
									break;
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
							}
						}
					;
					
					Params.m_fOnOutput
						= [&](NMib::NProcess::EProcessLaunchOutputType _OutputType, NMib::NStr::CStr const &_Output)
						{
							if (_OutputType == NMib::NProcess::EProcessLaunchOutputType_StdErr)
							{
								DMibLock(Lock);
								StdOut += _Output;
							}
							else
							{
								DMibDTrace("Unexpected output: {}\n", _Output);
							}
						}
					;
					{
						auto pLauncher = f_CreateLaunch<t_ProxyType != EProxyType_None>(Params, NMib::NProcess::EProcessLaunchCloseFlag_BlockOnExit);
					}
					
					{
						DMibTestPath("Block on exit");
						DMibTest(DMibExpr(Exited) == DMibExpr(EExitResult_Exited));
						DMibTest(DMibExpr(StdOut == fp_GetLongOutput()));
						#if 0
						if (StdOut != fp_GetLongOutput())
						{
							NMib::NFile::CFile::fs_WriteStringToFile(NMib::NStr::CStr("/Temp/Actual.txt"), StdOut);
							NMib::NFile::CFile::fs_WriteStringToFile(NMib::NStr::CStr("/Temp/Expected.txt"), fp_GetLongOutput());
						}
						#endif
						DMibTest(DMibExpr(ExitCode) == DMibExpr(m_ExitCode.f_Get()));
					}

				}
			};
		}
		
		template <EProxyType t_ProxyType>
		void f_TestWebLaunch()
		{
			DMibTestSuite("WebLaunch")
			{
				NMib::NAtomic::TCAtomic<int32> Finished;
				NMib::NThread::CEventAutoReset Event;
				NMib::NProcess::CProcessLaunchParams Params = NMib::NProcess::CProcessLaunchParams::fs_LaunchURL
					(
						""
						, "http://localhost"
						, [&](NMib::NProcess::CProcessLaunchStateChangeVariant const &_State, fp64 _TimeSinceStart)
						{
							
							switch (_State.f_GetTypeID())
							{
								case NMib::NProcess::EProcessLaunchState_Exited:
								{
									++Finished;
									Event.f_Signal();
								}
								break;
								case NMib::NProcess::EProcessLaunchState_LaunchFailed:
								{
									DMibTrace("Error: {}\r\n", _State.f_Get<NMib::NProcess::EProcessLaunchState_LaunchFailed>());
									++Finished;
									Event.f_Signal();
								}
								break;
							}
						}
					)
				;
				
				NMib::NContainer::TCVector<NMib::NPtr::TCUniquePointer<typename TCGetProxiedType<t_ProxyType>::CType>> Launches;
				mint nCores = NMib::NSys::fg_Thread_GetVirtualCores();
				for (mint i = 0; i < nCores * 2; ++i)
					Launches.f_Insert(f_CreateLaunch<t_ProxyType != EProxyType_None>(Params, NMib::NProcess::EProcessLaunchCloseFlag_LingerUntilDone));

				Launches.f_Clear();
				while (Finished.f_Load() != nCores*2)
				{
					Event.f_Wait();
				}
				
			};
		}
		
		template <EProxyType t_ProxyType>
		void f_TestSimpleURLLaunch()
		{
			DMibTestSuite(NMib::NTest::CTestCategory("SimpleURL") << NMib::NTest::CTestGroup("Manual"))
			{
				EExitResult Exited = EExitResult_None;
				bint Failed = false;
				uint32 ExitCode = 33;
				NMib::NStr::CStr FailedMessage;
				
				NMib::NProcess::CProcessLaunchParams Params = NMib::NProcess::CProcessLaunchParams::fs_LaunchURL
					(
						""
						, "http://localhost"
						, [&](NMib::NProcess::CProcessLaunchStateChangeVariant const &_State, fp64 _TimeSinceStart)
						{
							
							switch (_State.f_GetTypeID())
							{
							case NMib::NProcess::EProcessLaunchState_Exited:
								{
									NMib::fg_Volatile(Exited) = EExitResult_Exited;
									ExitCode = _State.f_Get<NMib::NProcess::EProcessLaunchState_Exited>();
								}
								break;
							case NMib::NProcess::EProcessLaunchState_LaunchFailed:
								{
									Failed = true;
									NMib::fg_Volatile(Exited) = EExitResult_NotLaunched;
									FailedMessage = _State.f_Get<NMib::NProcess::EProcessLaunchState_LaunchFailed>();
								}
								break;
							}
						}
					)
				;
				
				
				{
					DMibTestPath("Success");
					
					Params.m_Target = "http://www.hansoft.com";
					Exited = EExitResult_None;
					ExitCode = 66;
					NMib::NAtomic::fg_MemoryFence();
					
					{
						auto Launch = f_CreateLaunch<t_ProxyType != EProxyType_None>(Params, NMib::NProcess::EProcessLaunchCloseFlag_BlockOnExit);
					}
					
					#ifdef DPlatformFamily_Linux
						if (t_ProxyType == EProxyType_ElevatedProxied)
						{
							DMibTest(DMibExpr(Failed));
							DMibTest(DMibExpr(FailedMessage) == DMibExpr("Launching a document or URL elevated is not supported on Linux."));
						}
						else
					#endif
						{
							DMibTest(!DMibExpr(Failed));
							DMibTest(DMibExpr(ExitCode) == DMibExpr(0));
							DMibTest(DMibExpr(FailedMessage) == DMibExpr(""));
						}
				}
				
				{
					DMibTestPath("Failed");

					Params.m_Target = "failed://www.hansoft.com";
					Exited = EExitResult_None;
					ExitCode = 66;
					FailedMessage.f_Clear();
					NMib::NAtomic::fg_MemoryFence();
					
					{
						auto Launch = f_CreateLaunch<t_ProxyType != EProxyType_None>(Params, NMib::NProcess::EProcessLaunchCloseFlag_BlockOnExit);
					}
					
					#ifdef DPlatformFamily_Linux
						if (t_ProxyType == EProxyType_ElevatedProxied)
						{
							DMibTest(DMibExpr(Failed));
							DMibTest(DMibExpr(FailedMessage) == DMibExpr("Launching a document or URL elevated is not supported on Linux."));
						}
						else
					#endif
						{
							DMibTest(DMibExpr(Failed));
							DMibTest(DMibExpr(FailedMessage) != DMibExpr(""));
						}
				}
				
				
				
				{
					DMibTestPath("Linger");
					
					Params.m_Target = "http://www.hansoft.com";
					Exited = EExitResult_None;
					ExitCode = 66;
					FailedMessage.f_Clear();
					NMib::NAtomic::fg_MemoryFence();
					
					
					{
						auto pLauncher = f_CreateLaunch<t_ProxyType != EProxyType_None>(Params, NMib::NProcess::EProcessLaunchCloseFlag_LingerUntilDone);
					}
					
					while (NMib::fg_Volatile(Exited) == EExitResult_None)
					{
						NMib::NAtomic::fg_MemoryFence(NMib::NAtomic::EMemoryOrder_Acquire);
						NMib::NSys::fg_Thread_SmallestSleep();
					}
					#ifdef DPlatformFamily_Linux
						if (t_ProxyType == EProxyType_ElevatedProxied)
						{
							DMibTest(DMibExpr(Failed));
							DMibTest(DMibExpr(FailedMessage) == DMibExpr("Launching a document or URL elevated is not supported on Linux."));
							DMibTrace("FailedMessage = \"{}\"", FailedMessage);
							DMibTraceRaw("Expect = \"Launching a document or URL elevated is not supported on Linux.\"");
						}
						else
					#endif
						{
							DMibTest(DMibExpr(Exited) == DMibExpr(EExitResult_Exited));
							DMibTest(DMibExpr(ExitCode) == DMibExpr(0));
							DMibTest(DMibExpr(FailedMessage) == DMibExpr(""));
						}
				}
				
				{
					DMibTestPath("Dispatcher");
					
					NMib::NContainer::TCThreadSafeQueue<NMib::NFunction::TCFunction<void ()>> DispatchQueue;
					
					Params.m_Target = "http://www.hansoft.com";
					Exited = EExitResult_None;
					ExitCode = 66;
					FailedMessage.f_Clear();
					Params.m_fDispatcher
							= [&](NMib::NFunction::TCFunction<void ()> const &_ToDispatch)
						{
							DispatchQueue.f_Push(_ToDispatch);
						}
					;
					
					NMib::NAtomic::fg_MemoryFence();
					
					{
						auto pLauncher = f_CreateLaunch<t_ProxyType != EProxyType_None>(Params, NMib::NProcess::EProcessLaunchCloseFlag_LingerUntilDone);
					}
					while (NMib::fg_Volatile(Exited) == EExitResult_None)
					{
						NMib::NAtomic::fg_MemoryFence(NMib::NAtomic::EMemoryOrder_Acquire);
						NMib::NSys::fg_Thread_SmallestSleep();
						while (auto QueueEntry = DispatchQueue.f_Pop())
						{
							(*QueueEntry)();
						}
					}
					

					#ifdef DPlatformFamily_Linux
						if (t_ProxyType == EProxyType_ElevatedProxied)
						{
							DMibTest(DMibExpr(Failed));
							DMibTest(DMibExpr(FailedMessage) == DMibExpr("Launching a document or URL elevated is not supported on Linux."));
						}
						else
					#endif
						{
							DMibTest(DMibExpr(Exited) == DMibExpr(EExitResult_Exited));
							DMibTest(DMibExpr(ExitCode) == DMibExpr(0));
							DMibTest(DMibExpr(FailedMessage) == DMibExpr(""));
						}
				}
			};
		}
		
		template <EProxyType t_ProxyType>
		void f_TestProxyFailure(bint _bThreaded)
		{
			DMibTestSuite((_bThreaded ? "Proxy failure" : "Proxy failure non threaded"))
			{
				if (fg_TestReportFlags() & ETestReportFlag_ProcessRecursive)
				{
				}
				else
				{
					EExitResult Exited = EExitResult_None;
					uint32 ExitCode = 66;
					NMib::NStr::CStr StdOut;
					NMib::NProcess::CProcessLaunchParams Params = f_GetLaunchParams();
					Params.m_bThreaded = _bThreaded;
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
						auto pLauncher = f_CreateLaunch<t_ProxyType != EProxyType_None>(Params, NMib::NProcess::EProcessLaunchCloseFlag_BlockOnExit);
						if (!_bThreaded)
							pLauncher->f_Start();
					}
					
					{
						DMibTestPath("Block on exit");
						DMibTest(DMibExpr(Exited) == DMibExpr(EExitResult_NotLaunched));
					}

					Exited = EExitResult_None;
					ExitCode = 66;
					StdOut.f_Clear();
					NMib::NAtomic::fg_MemoryFence();

					{
						auto pLauncher = f_CreateLaunch<t_ProxyType != EProxyType_None>(Params, NMib::NProcess::EProcessLaunchCloseFlag_LingerUntilDone);
						if (!_bThreaded)
							pLauncher->f_Start();
					}

					while (NMib::fg_Volatile(Exited) == EExitResult_None)
					{
						NMib::NAtomic::fg_MemoryFence(NMib::NAtomic::EMemoryOrder_Acquire);
						NMib::NSys::fg_Thread_SmallestSleep();
					}
					{
						DMibTestPath("Wait for exit");
						DMibTest(DMibExpr(Exited) == DMibExpr(EExitResult_NotLaunched));
					}

					Exited = EExitResult_None;
					ExitCode = 66;
					StdOut.f_Clear();
					NMib::NAtomic::fg_MemoryFence();

					NMib::NContainer::TCThreadSafeQueue<NMib::NFunction::TCFunction<void ()>> DispatchQueue;
					
					Params.m_fDispatcher
						= [&](NMib::NFunction::TCFunction<void ()> const &_ToDispatch)
						{
							DispatchQueue.f_Push(_ToDispatch);
						}
					;


					{
						auto pLauncher = f_CreateLaunch<t_ProxyType != EProxyType_None>(Params, NMib::NProcess::EProcessLaunchCloseFlag_LingerUntilDone);
						if (!_bThreaded)
							pLauncher->f_Start();
					}

					while (NMib::fg_Volatile(Exited) == EExitResult_None)
					{
						NMib::NAtomic::fg_MemoryFence(NMib::NAtomic::EMemoryOrder_Acquire);
						NMib::NSys::fg_Thread_SmallestSleep();
						while (auto QueueEntry = DispatchQueue.f_Pop())
						{
							(*QueueEntry)();
						}
					}
					{
						DMibTestPath("Wait for exit with dispatch");
						DMibTest(DMibExpr(Exited) == DMibExpr(EExitResult_NotLaunched));
					}
				}
			};
		}
	
		template <EProxyType t_ProxyType>
		void f_TestKillSandbox()
		{
			if (fg_TestReportFlags() & ETestReportFlag_ProcessRecursive)
			{
				DMibTestCategory("KillSandbox")
				{
					for (mint i = 0; i < 10; ++i)
					{
						NMib::NStr::CStr Suite = NMib::NStr::CStr::fs_ToStr(i);
						DMibTestSuite(Suite)
						{
							NMib::NStr::CStr FileToOpen = NMib::fg_GetSys()->f_GetEnvironmentVariable("LockFileDir");
							NMib::NFile::CFile::fs_CreateDirectory(FileToOpen);
							auto FileDir = FileToOpen;
							FileToOpen += NMib::NStr::CStr::CFormat("/{}.FileOpen") << i;
							
							NMib::NFile::CFile File;
							File.f_Open(FileToOpen, NMib::NFile::EFileOpen_Write);
							File.f_SetAttributes
								(
									File.f_GetAttributes() 
									| NMib::NFile::CFile::fs_GetValidAttributes() 
									| NMib::NFile::EFileAttrib_EveryoneWrite 
									| NMib::NFile::EFileAttrib_GroupWrite 
									| NMib::NFile::EFileAttrib_UserWrite
								)
							;
							
#ifndef DPlatformFamily_Windows
							NMib::NStr::CStr User = NMib::fg_GetSys()->f_GetEnvironmentVariable("MalterlibOriginalUser");
							NMib::NStr::CStr Group = NMib::fg_GetSys()->f_GetEnvironmentVariable("MalterlibOriginalGroup");
							
							NMib::NFile::CFile::fs_SetOwner(FileToOpen, User);
							NMib::NFile::CFile::fs_SetGroup(FileToOpen, Group);
							NMib::NFile::CFile::fs_SetOwner(FileDir, User);
							NMib::NFile::CFile::fs_SetGroup(FileDir, Group);
#endif
							
							if (i == 0)
							{
								while (true)
								{
									NMib::NSys::fg_Thread_Sleep(1.0);
								}
							}
							NMib::NStr::CStr NextPath = fs_GetTestPath(NMib::NFile::CFile::fs_GetPath(fg_TestGetCurrentPath()), NMib::NStr::CStr::fs_ToStr(i - 1), "Null");
							NMib::NStr::CStr StdOut;
							NMib::NStr::CStr StdErr;
							uint32 ExitCode;
							NMib::NProcess::CProcessLaunch::fs_LaunchBlock
								(
									NMib::NFile::CFile::fs_GetProgramPath()
									, NextPath
									, StdOut
									, StdErr
									, ExitCode
								)
							;
							
							if (!StdErr.f_IsEmpty())
								DMibConErrOutRaw(StdErr);
						};
					}
				};
			}
			else
			{
				DMibTestSuite("KillSandbox")
				{
					EExitResult Exited = EExitResult_None;
					uint32 ExitCode = 66;
					NMib::NStr::CStr StdOut;

					NMib::NProcess::CProcessLaunchParams Params = f_GetLaunchParams(fg_TestGetCurrentPath() + "/9", "", "Null");

					Params.m_bSandboxed = true;
					
					NMib::NStr::CStr TempDirectory = NMib::NFile::CFile::fs_GetTemporaryDirectory() + "/MalterlibProcessKill/" + NMib::NDataProcessing::fg_GetRandomUuidString();
					NMib::NFile::CFile::fs_CreateDirectory(TempDirectory);
					Params.m_Environment["LockFileDir"] = TempDirectory;
					Params.m_Environment["MalterlibOriginalUser"] = NMib::NSys::fg_UserManagement_GetProcessRealUserName();
					Params.m_Environment["MalterlibOriginalGroup"] = NMib::NSys::fg_UserManagement_GetProcessRealGroupName();

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
							}
						}
					;
					
					NMib::NStr::CStr StdErr;

					Params.m_fOnOutput
						= [&](NMib::NProcess::EProcessLaunchOutputType _OutputType, NMib::NStr::CStr const &_Output)
						{
							if (_OutputType == NMib::NProcess::EProcessLaunchOutputType_StdOut)
								StdOut += _Output;
							else
								StdErr += _Output;
						}
					;

					{
						auto pLauncher = f_CreateLaunch<t_ProxyType != EProxyType_None>(Params, NMib::NProcess::EProcessLaunchCloseFlag_BlockOnExit | NMib::NProcess::EProcessLaunchCloseFlag_TerminateProcess);

						// Wait until all child processes are launched
						NMib::NTime::CClock Clock;
						Clock.f_Start();
						bool bTimedOutCreated = true;
						while (bTimedOutCreated && Clock.f_GetTime() < 100.0)
						{
							bTimedOutCreated = false;
							for (int i = 0; i < 10; ++i)
							{
								NMib::NStr::CStr FileToOpen = TempDirectory + NMib::NStr::CStr(NMib::NStr::CStr::CFormat("/{}.FileOpen") << i);
								
								if (!NMib::NFile::CFile::fs_FileExists(FileToOpen))
								{
									bTimedOutCreated = true;
									break;
								}								
							}
						}
						
						DMibTest(!DMibExpr(bTimedOutCreated));
					}

					// Terminate, and wait for child processes to terminate
					
					// Wait until all child processes are launched
					{
						NMib::NTime::CClock Clock;
						Clock.f_Start();
						bool bTimedOutTerminate = true;
						while (bTimedOutTerminate && Clock.f_GetTime() < 100.0)
						{
							bTimedOutTerminate = false;
							for (int i = 0; i < 10; ++i)
							{
								NMib::NStr::CStr FileToOpen = TempDirectory + NMib::NStr::CStr(NMib::NStr::CStr::CFormat("/{}.FileOpen") << i);
								
								if (NMib::NFile::CFile::fs_FileExists(FileToOpen))
								{
									try
									{
										{
											NMib::NFile::CFile File;
											File.f_Open(FileToOpen, NMib::NFile::EFileOpen_Write);
										}
										NMib::NFile::CFile::fs_DeleteFile(FileToOpen);
									}
									catch (NMib::NFile::CExceptionFile const &)
									{
										bTimedOutTerminate = true;
										break;
									}
								}								
							}
						}
						try
						{
							NMib::NFile::CFile::fs_DeleteDirectory(TempDirectory);
						}
						catch (NMib::NFile::CExceptionFile const &)
						{
						}
						
						DMibTest(!DMibExpr(bTimedOutTerminate));
					}
					

					{
						DMibTestPath("Block on exit");
						DMibTest(DMibExpr(Exited) == DMibExpr(EExitResult_Exited));
						DMibTest(DMibExpr(ExitCode) == DMibExpr(255));
#ifndef DPlatformFamily_Windows
						DMibTest(DMibExpr(StdErr) == DMibExpr("Process terminated due to signal 9\n")); // (ETestFlag_NoValues);
#endif
					}
				};
			}
		}
	

		template <EProxyType t_ProxyType>
		void f_DoAllTests()
		{
			f_EnableClientFailure(false);
			
			NMib::NStr::CStr Desc = "Normal";
			if (t_ProxyType == EProxyType_Proxied)
				Desc = "Proxied";
			else if (t_ProxyType == EProxyType_ElevatedProxied)
				Desc = "Proxied Elevated";

			NMib::NTest::CTestCategory TestCategory(Desc);

			if (t_ProxyType == EProxyType_ElevatedProxied)
				TestCategory << NMib::NTest::CTestGroup("Manual");
			
			DMibTestCategory(TestCategory)
			{
				f_TestExecutable<t_ProxyType>(true);
				f_TestExecutable<t_ProxyType>(false);
	#ifdef DPlatformFamily_Windows
				f_TestDocument<t_ProxyType>();
	#endif
				f_TestElevation<t_ProxyType>();
				if (t_ProxyType == EProxyType_None)
					f_TestElevationStdIn();
					
				f_TestDeElevation<t_ProxyType>();
				f_TestPerformance<t_ProxyType>(true);
				f_TestPerformance<t_ProxyType>(false);
	#ifdef DPlatformFamily_Windows
				f_TestSandbox<t_ProxyType>();
	#endif
				f_TestKillSandbox<t_ProxyType>();
				f_TestExecutableFromDll<t_ProxyType>();
#ifndef DPlatformFamily_Linux // Not supported on linux for now
				f_TestLimits<t_ProxyType>();
#endif
				f_TestStdOut<t_ProxyType>();
				f_TestSimpleURLLaunch<t_ProxyType>();
				
				if (t_ProxyType != EProxyType_None)
				{
					f_EnableClientFailure(true);
					f_TestProxyFailure<t_ProxyType>(true);
					f_TestProxyFailure<t_ProxyType>(false);
				}
				//f_TestWebLaunch();
			};
		}
		
		void f_DoTests()
		{
			f_DoProxyServer();
			
			for (auto iTest = 0; iTest < 2; ++iTest)
			{
				NMib::NStr::CStr Path = "ExitCode0";
				if (iTest == 1)
				{
					Path = "ExitCode33";
					m_ExitCode = 33;
				}
				DMibTestCategory(Path)
				{
					m_bProxyElevation = false;
					f_DoAllTests<EProxyType_None>();
					f_DoAllTests<EProxyType_Proxied>();
					m_bProxyElevation = true;
					f_DoAllTests<EProxyType_ElevatedProxied>();
				};
			}
		}
	};

	DMibTestRegister(CProcessLaunch_Tests, Malterlib::Process);

}

