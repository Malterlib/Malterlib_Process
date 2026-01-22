# CLAUDE.md - Process Module

This file provides guidance to Claude Code (claude.ai/code) when working with the Process module in Malterlib.

## Module Overview

The Process module provides comprehensive cross-platform process management capabilities including:
- Process launching and control (synchronous and asynchronous)
- Standard I/O redirection and handling
- Process elevation/de-elevation (admin/root privileges)
- Process sandboxing and resource limits
- Process monitoring and statistics
- Virtual process abstraction for testing
- Proxied process launching for elevated operations
- StdIn reading and interactive console input

## Architecture

### Core Components

#### CProcessLaunch
Main class for launching and managing processes. Provides synchronous process execution with callbacks for output and state changes.

```cpp
// Basic process launch
NProcess::CProcessLaunchParams Params;
Params.m_Target = "/usr/bin/ls";
Params.m_Parameters = "-la";
Params.m_WorkingDirectory = "/tmp";
Params.m_fOnOutput = [](EProcessLaunchOutputType _Type, NStr::CStr const &_Output)
{
	if (_Type == EProcessLaunchOutputType_StdOut)
		DMibLog("Output: {}", _Output);
};
Params.m_fOnStateChange = [](CProcessLaunchStateChangeVariant const &_State, fp64 _TimeSinceStart)
{
	if (_State.f_GetType() == EProcessLaunchState_Exited)
	{
		uint32 ExitCode = _State.f_Get<EProcessLaunchState_Exited>();
		DMibLog("Process exited with code: {}", ExitCode);
	}
};

NProcess::CProcessLaunch Launch(Params);
```

#### CProcessLaunchActor
Actor-based asynchronous process execution using the Concurrency module. Ideal for non-blocking process management.

```cpp
// Asynchronous process launch with actor
NConcurrency::TCFuture<void> f_LaunchExample()
{
	NConcurrency::TCActor<NProcess::CProcessLaunchActor> Actor = fg_Construct();

	// Simple launch with result
	auto Result = co_await Actor.f_LaunchSimple
		(
			NProcess::CProcessLaunchActor::CSimpleLaunch
			(
				"/usr/bin/echo",
				{"Hello", "World"},
				"/tmp"
			)
		)
	;

	DMibLog("Exit code: {}", Result.m_ExitCode);
	DMibLog("Output: {}", Result.f_GetStdOut());

	// Complex launch with callbacks
	NProcess::CProcessLaunchActor::CLaunch LaunchConfig(Params);
	LaunchConfig.m_ToLog = NProcess::CProcessLaunchActor::ELogFlag_All;
	LaunchConfig.m_LogName = "MyProcess";

	auto Subscription = co_await Actor(&NProcess::CProcessLaunchActor::f_Launch, LaunchConfig, fg_ThisActor(this));
}
```

#### CStdInReader
Handles reading from standard input with support for both blocking and non-blocking modes, including password input. Should not be used, available for legacy use cases, use the actor version.

#### CStdInActor
Actor-based stdin handling for asynchronous input processing.

```cpp
NConcurrency::TCFuture<void> f_ReadInputExample()
{
	NConcurrency::TCActor<NProcess::CStdInActor> StdInActor;
	NStr::CStr Name = co_await StdInActor.f_ReadLine();
}
```

### Platform Support

#### Platform-Specific Files
- **Windows**: `Platform/Malterlib_Process_Platform_Windows*.cpp`
  - Uses CreateProcess API
  - Supports elevation via UAC
  - Job objects for process groups
- **macOS**: `Platform/Malterlib_Process_Platform_MacOS*.cpp/mm`
  - Uses posix_spawn
  - AuthorizationExecuteWithPrivileges for elevation
  - launchctl for user session launches
- **Linux**: `Platform/Malterlib_Process_Platform_Linux*.cpp`
  - Uses fork/exec
  - Resource limits via setrlimit
  - Process capabilities support
- **POSIX Common**: `Platform/Malterlib_Process_Platform_POSIX*.cpp`
  - Shared Unix functionality
  - Signal handling
  - Process groups

### Key Enumerations

#### EProcessLaunchOutputType
```cpp
EProcessLaunchOutputType_StdOut        // Standard output
EProcessLaunchOutputType_StdErr        // Standard error
EProcessLaunchOutputType_GeneralError  // Launch or system errors
EProcessLaunchOutputType_TerminateMessage // Process termination info
```

#### EProcessLaunchElevation
```cpp
EProcessLaunchElevation_None      // Run with current privileges
EProcessLaunchElevation_Elevate   // Request admin/root privileges
EProcessLaunchElevation_DeElevate // Drop privileges
```

#### EProcessLaunchState
```cpp
EProcessLaunchState_Launched     // Process started successfully
EProcessLaunchState_LaunchFailed // Failed to start process
EProcessLaunchState_Exited       // Process terminated
```

#### EProcessLaunchType
```cpp
EProcessLaunchType_Executable // Launch a program
EProcessLaunchType_Document   // Open a document with default app
EProcessLaunchType_URL        // Open URL in default browser
```

## Common Usage Patterns

### Asynchronous Process with Timeout
```cpp
NConcurrency::TCFuture<bool> f_RunWithTimeout(NStr::CStr _Exe, fp64 _Timeout)
{
	NConcurrency::TCActor<NProcess::CProcessLaunchActor> Actor = fg_Construct();

	// You will automatically return an exception when timeout occurs
	auto Result = co_await Actor(&NProcess::CProcessLaunchActor::f_LaunchSimple, NProcess::CProcessLaunchActor::CSimpleLaunch(_Exe)).f_Timeout(_Timeout, "Timed out waiting for launch");

	co_await fg_Move(Actor).f_Destruct();

	// Race between completion and timeout
	co_return Result.m_ExitCode != 0;
}
```

### Interactive Process Communication
```cpp
class CInteractiveProcess
{
	NConcurrency::TCActor<NProcess::CProcessLaunchActor> m_Actor;
	NConcurrency::CActorSubscription m_Subscription;

public:
	NConcurrency::TCFuture<void> f_Start()
	{
		m_Actor = fg_Construct();

		NProcess::CProcessLaunchParams Params;
		Params.m_Target = "/usr/bin/python3";
		Params.m_Parameters = "-i"; // Interactive mode
		Params.m_bEnableStdRedirection = true;

		NProcess::CProcessLaunchActor::CLaunch Launch(Params);

		m_Subscription = co_await m_Actor(&NProcess::CProcessLaunchActor::f_Launch, Launch, fg_ThisActor(this));
	}

	NConcurrency::TCFuture<void> f_SendCommand(NStr::CStr _Command)
	{
		co_return co_await m_Actor(&NProcess::CProcessLaunchActor::f_SendStdIn, _Command + "\n");
	}

	NConcurrency::TCFuture<uint32> f_Stop()
	{
		co_return co_await m_Actor(&NProcess::CProcessLaunchActor::f_StopProcess);
	}
};
```

## Testing

### Test Structure
Tests are located in the `Test/` directory:
- `Test_Malterlib_Process_ProcessLaunch.cpp` - Core launch functionality
- `Test_Malterlib_Process_ProcessLaunchFeatures.cpp` - Advanced features (elevation, sandboxing)
- `Test_Malterlib_Process_StdIn.cpp` - Standard input handling
- `Test_Malterlib_Process_Info.cpp` - Process information and enumeration

### Helper Applications
The module includes helper executables for testing:
- `Malterlib_Helper_Process` - UI executable for testing launches
- `Test_Malterlib_Helper_Process.dll` - Dynamic library for testing

### Running Tests
```bash
# Bulid all tests and run all Process module tests
MalterlibBuildShowProgress=false ./mib test --paths '["Malterlib/Process/*"]'

# Run process tests
/opt/Deploy/Tests/RunAllTests --paths '["Malterlib/Process/*"]'

```

### Testing Best Practices
1. Use `CProcessLaunchActor` for test isolation
2. Mock process behavior with `CVirtualProcessLaunch`
3. Test platform-specific behavior with appropriate guards
4. Verify resource cleanup with `EProcessLaunchCloseFlag`
5. Test elevation only when appropriate permissions exist

## Build Configuration

### Module Properties
```mib
Property
{
	MalterlibLibrary_Process: bool = MalterlibLibrary_All
	MalterlibSubLibraries =+ "Process"
}
```

### Dependencies
- **Core**: Core Malterlib functionality
- **Concurrency**: Actor system and async operations (indirect)
- **Security.framework**: macOS elevation support

### Platform-Specific Compilation
Files in `Platform/` directory are conditionally compiled based on `PlatformFamilyFromFile`:
- `*_Windows.cpp` - Windows only
- `*_MacOS.cpp/mm` - macOS only
- `*_Linux.cpp` - Linux only
- `*_POSIX.cpp` - All Unix platforms

## Security Considerations

### Process Elevation
- Always validate elevation requests
- Use `m_IconPath` and `m_Prompt` on macOS for user clarity
- Consider using `CProxiedProcessLaunch` for privilege separation
- Never store passwords in `m_RunAsUserPassword` longer than necessary

### Sandboxing
```cpp
Params.m_bSandboxed = true;
Params.m_SandboxRoots["/"] = "/sandbox/root"; // Unix chroot
Params.m_SandboxRoots["C:"] = "S:\\sandbox"; // Windows drive remapping
Params.m_bCopyRootToSandbox = true; // Copy files to sandbox
```

## Performance Guidelines

### Optimization Tips
2. Enable `m_bSeparateStdErr = false` if stderr not needed
3. Use `m_bEnableStdRedirection = false` for fire-and-forget processes

### Memory Management
- Process handles are automatically cleaned up
- Use `EProcessLaunchCloseFlag_BlockOnExit` for synchronous cleanup
- Actor-based launches manage lifetime automatically

## Common Issues and Solutions

### Issue: Elevation prompts not appearing
**Solution**: Set `m_bShowLaunched = true` and `m_bAllowLaunchedInForeground = true`

### Issue: Environment variables not inherited
**Solution**: Set `m_bMergeEnvironment = true`

### Issue: Can't find executable
**Solution**: Set `m_bAllowExecutableLocate = true` to search PATH

### Issue: Process killed immediately on Unix
**Solution**: Check resource limits and signal masks

## Module-Specific Conventions

### Error Handling
```cpp
try
{
	NProcess::CProcessLaunch Launch(Params);
}
catch (NProcess::CExceptionProcessLaunch const &_Ex)
{
	DMibLog("Launch failed: {}", _Ex.f_GetMessage());
}
```

### Logging
Use the built-in logging flags with `CProcessLaunchActor`:
```cpp
Launch.m_ToLog = ELogFlag_Error | ELogFlag_StdOut;
Launch.m_LogName = "MyProcess";
```

### Thread Safety
- `CProcessLaunch` is not thread-safe, use one per thread
- `CProcessLaunchActor` is fully thread-safe
- `CStdInReader` with `EStdInReaderFlag_Exclusive` ensures single reader
- Platform functions in `NPlatform` namespace are thread-safe
