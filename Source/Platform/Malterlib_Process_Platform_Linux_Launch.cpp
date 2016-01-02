// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Core/PlatformSpecific/PosixErrNo>
#include <Mib/Cryptography/UUID>
#include <Mib/Desktop/DesktopFile>

#include "../Malterlib_Process_Platform.h"

#include "Malterlib_Process_Platform_POSIX.h"
#include "Malterlib_Process_Platform_POSIX_PlatformSpecific.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

bool NMib::NProcess::NPlatform::fg_Linux_LaunchExecutableWithRoot(NMib::NProcess::NPlatform::CPOSIXLaunchContext *_pLaunchContext, NMib::NProcess::CProcessLaunchParams &_Params, NMib::NStr::CStr &_Program, int& _StdErrRead, int& _StdErrWrite, int &_StdInRead, int &_StdInWrite, NMib::NStr::CStr &_Errors)
{
	bint bGKSUAvailable = fg_FindExecutable("gksu", true, NMib::NFile::EFileAttrib_File|NMib::NFile::EFileAttrib_Executable) != "gksu";
	if (bGKSUAvailable)
		_Program = "gksu";
	else
	{
		// Fall back to gnomesu.
		bint bGNomeSUAvailable = fg_FindExecutable("gnomesu", true, NMib::NFile::EFileAttrib_File|NMib::NFile::EFileAttrib_Executable) != "gnomesu";
		if (bGNomeSUAvailable)
			_Program = "gnomesu";
		else
		{
			_Errors += "Could not find either gksu or gnomesu to perform elevation.\n";
			return false;
		}
	}
			
	NStr::CStr Executable = _Params.m_Target;
	NStr::CStr InPipeName;
	
	NStr::CStr Parameters;
	if (_Params.m_bStdOutPID)
	{
		if (_StdErrWrite != -1)
		{
			close(_StdErrWrite);
			_StdErrWrite = -1;
		}
		
		if (_StdErrRead != -1)
		{
			close(_StdErrRead);
			_StdErrRead = -1;
		}
		
		if (_StdInRead != -1)
		{
			close(_StdInRead);
			_StdInRead = -1;
		}
		
		if (_StdInWrite != -1)
		{
			close(_StdInWrite);
			_StdInWrite = -1;
		}
		
		NStr::CStr RandomString = NMib::NDataProcessing::fg_GetSecureUuidString();
		
		NStr::CStr ReadPipeName = NMib::NFile::CFile::fs_GetUserLocalProgramDirectory() + "/NamePipeStdErr_" + RandomString;
		InPipeName = NMib::NFile::CFile::fs_GetUserLocalProgramDirectory() + "/NamePipeStdIn_" + RandomString;
		
		NMib::NFile::CFile::fs_CreateDirectory(NMib::NFile::CFile::fs_GetUserLocalProgramDirectory());
					
		if (mkfifo(ReadPipeName.f_GetStr(), S_IRUSR|S_IWUSR) == 0)
		{
			_StdErrRead = open(ReadPipeName.f_GetStr(), O_CLOEXEC|O_NONBLOCK|O_RDONLY, S_IRUSR);
							
			if (_StdErrRead == -1)
			{
				_Errors += NMib::NPlatform::fg_FormatErrno("open (elevate stderr pipe)", errno);
				return false;
			}
		}
		else
		{
			_Errors += NMib::NPlatform::fg_FormatErrno("mkfifo (elevate stderr pipe)", errno);
			return false;
		}
		
		if (mkfifo(InPipeName.f_GetStr(), S_IRUSR|S_IWUSR) != 0)
		{
			_Errors += NMib::NPlatform::fg_FormatErrno("mkfifo (elevate stdin pipe)", errno);
			return false;
		}
					
		Parameters = NStr::CStr::CFormat("\"--OutputPID {}/{}\" ") << NMib::NFile::CFile::fs_GetUserLocalProgramDirectory() << RandomString;
	}
	
	Parameters += _Params.m_Parameters;
	
	if (_Program == "gnomesu")
	{
		_Params.m_Parameters = "--";

		fg_AddStrSepEscaped(_Params.m_Parameters, Executable, ' ');
		
		if (!Parameters.f_IsEmpty())
		{
			_Params.m_Parameters += " ";
			_Params.m_Parameters += Parameters;
		}
	}
	else if (_Program == "gksu")
	{
		if (_Params.m_Prompt.f_IsEmpty())
		{
			NStr::CStr Target = _Params.m_Target;
			if (Target.f_StartsWith("/"))
				Target = (NStr::CStr::CFormat(" {}") << Target).f_GetStr();
		
			_Params.m_Parameters = "--description " + Target.f_EscapeStr();
		}
		else
		{
			NStr::CStr Prompt = _Params.m_Prompt;
			if (Prompt.f_StartsWith("/"))
				Prompt = (NStr::CStr::CFormat(" {}") << Prompt).f_GetStr();
			
			_Params.m_Parameters = "--message " + Prompt.f_EscapeStr();
		}

		_Params.m_Parameters += " --";
		fg_AddStrSepEscaped(_Params.m_Parameters, Executable, ' ');
				
		if (!Parameters.f_IsEmpty())
			_Params.m_Parameters += " " + Parameters;
	}
				
	_Params.m_bAllowExecutableLocate = true;
	_Params.m_Elevation = NProcess::EProcessLaunchElevation_None;
	_Params.m_bSeparateStdErr = true;
				
	struct CLaunchOutput
	{
		EProcessLaunchOutputType m_OutputType;
		NMib::NStr::CStr m_Output;
	};
	
	struct CLaunchData
	{
		CLaunchData()
			: m_pLaunchedProcess(nullptr)
		{
		}
		
		NMib::NProcess::NPlatform::CPOSIXLaunchContext *m_pLaunchContext;
		NContainer::TCVector<CLaunchOutput> m_LaunchOutputs;
		NStr::CStr m_QueuedStdOut;
		NStr::CStr m_StdInPipeName;
		zbint m_bReceivedPID;
		zbint m_bFailedStateChange;
		zbint m_bFailed;
		NStr::CStr m_Executable;
		NStr::CStr m_SavedError;
		
		NFunction::TCFunction<void (CProcessLaunchStateChangeVariant const &_State, fp64 _TimeSinceStart)> m_fOnStateChange;
		NFunction::TCFunction<void (EProcessLaunchOutputType _OutputType, NMib::NStr::CStr const &_Output)> m_fOnOutput;
		
		NMib::NThread::CMutual m_Lock;
		
		void * m_pLaunchedProcess;
	};
	
	NPtr::TCSharedPointer<CLaunchData> pLaunchData = fg_Construct();
	pLaunchData->m_bReceivedPID = !_Params.m_bStdOutPID;
	pLaunchData->m_fOnOutput = fg_Move(_Params.m_fOnOutput);
	pLaunchData->m_fOnStateChange = fg_Move(_Params.m_fOnStateChange);
	pLaunchData->m_StdInPipeName = fg_Move(InPipeName);
	pLaunchData->m_pLaunchContext = _pLaunchContext;
	pLaunchData->m_Executable = Executable;
	
	_Params.m_fOnStateChange =
		[pLaunchData](NMib::NProcess::CProcessLaunchStateChangeVariant const &_State, fp64 _TimeSinceStart)
		{
			DMibLock(pLaunchData->m_Lock);
								
			if (pLaunchData->m_bFailedStateChange)
				return;
							
			switch (_State.f_GetTypeID())
			{
			case NMib::NProcess::EProcessLaunchState_Exited:
				{						
					if (pLaunchData->m_bFailed || !pLaunchData->m_bReceivedPID)
					{
						NStr::CStr ErrorStr;
						
						for(auto iOutput = pLaunchData->m_LaunchOutputs.f_GetIterator(); iOutput; ++iOutput)
						{
							if (iOutput->m_OutputType != EProcessLaunchOutputType_StdOut)
								ErrorStr += iOutput->m_Output;
						}
						
						if (ErrorStr.f_IsEmpty())
						{
							if (_State.f_Get<NMib::NProcess::EProcessLaunchState_Exited>() == 1)
							{
								ErrorStr = "Could not locate executable: \"";
								ErrorStr += pLaunchData->m_Executable;
								ErrorStr += "\"";
							}
							else if (_State.f_Get<NMib::NProcess::EProcessLaunchState_Exited>() == 255)
								ErrorStr = "User aborted launch";
							else
								ErrorStr = NStr::CStr::CFormat("Unknown error ({})") << _State.f_Get<NMib::NProcess::EProcessLaunchState_Exited>();
							
							pLaunchData->m_SavedError = pLaunchData->m_SavedError.f_Trim();
							if (!pLaunchData->m_SavedError.f_IsEmpty())
							{
								ErrorStr += ": ";
								ErrorStr += pLaunchData->m_SavedError;
							}
						}
													
						pLaunchData->m_bFailedStateChange = true;
						
						if (pLaunchData->m_fOnStateChange)
							pLaunchData->m_fOnStateChange(ErrorStr, _TimeSinceStart);
					}
					else
					{							
						if (pLaunchData->m_fOnStateChange)
							pLaunchData->m_fOnStateChange(_State, _TimeSinceStart);
					}
				}
				break;
					
			case NMib::NProcess::EProcessLaunchState_Launched:
				{							
					if (pLaunchData->m_bReceivedPID)
					{
						if (pLaunchData->m_fOnStateChange)
							pLaunchData->m_fOnStateChange(_State, _TimeSinceStart);
					}
					else
					{
						pLaunchData->m_pLaunchedProcess = _State.f_Get<NMib::NProcess::EProcessLaunchState_Launched>();
					}
				}
				break;
					
			case NMib::NProcess::EProcessLaunchState_LaunchFailed:
				{
					pLaunchData->m_bFailedStateChange = true;
					
					if (pLaunchData->m_fOnStateChange)
						pLaunchData->m_fOnStateChange(_State, _TimeSinceStart);
				}
				break;
			}
		}
	;
			
	_Params.m_fOnOutput =
		[pLaunchData](EProcessLaunchOutputType _OutputType, NMib::NStr::CStr const &_Output)
		{
			DMibLock(pLaunchData->m_Lock);

			if (pLaunchData->m_bFailedStateChange)
				return;
			
			if (pLaunchData->m_bReceivedPID)
			{
				if (pLaunchData->m_fOnOutput)
					pLaunchData->m_fOnOutput(_OutputType, _Output);
			}
			else
			{
				if (_OutputType != EProcessLaunchOutputType_StdOut || pLaunchData->m_bFailed)
				{
					auto & Output = pLaunchData->m_LaunchOutputs.f_Insert();
					
					Output.m_OutputType = _OutputType;
					Output.m_Output = _Output;
				}
				else
				{
					pLaunchData->m_QueuedStdOut += _Output;
					
					while (true)
					{
						aint iLine = pLaunchData->m_QueuedStdOut.f_FindChar('\n');
						if (iLine < 0)
						{
							return;
						}
						
						NStr::CStr Line = pLaunchData->m_QueuedStdOut.f_Left(iLine);
						pLaunchData->m_QueuedStdOut = pLaunchData->m_QueuedStdOut.f_Extract(iLine + 1);
						
						if (Line.f_StartsWith("bdda0079-b6eb-41ac-88d0-01b50e8be939 "))
						//if (pLaunchData->m_QueuedStdOut.f_GetLen() >= ExpectedLength + 17)
						{
							fg_GetStrSep(Line, " ");
							NStr::CStr Pid = fg_GetStrSep(Line, " ");
							NStr::CStr Error = Line.f_ReplaceChar('\r', '\n').f_Trim();
							//CStr ToParse = pLaunchData->m_QueuedStdOut.f_Left(ExpectedLength + 17);
							pLaunchData->m_bReceivedPID = true;
							
							if (!Error.f_IsEmpty() && !pLaunchData->m_bFailedStateChange)
							{
								if (pLaunchData->m_fOnStateChange)
									pLaunchData->m_fOnStateChange(Error, 0.0);
								pLaunchData->m_bFailedStateChange = true;
								return ;
							}
							
														
							if (!pLaunchData->m_StdInPipeName.f_IsEmpty())
							{								
								int Pipe = open(pLaunchData->m_StdInPipeName.f_GetStr(), O_CLOEXEC|O_WRONLY, S_IWUSR);
															
								if (Pipe == -1)
								{									
									if (!pLaunchData->m_bFailedStateChange)
									{	
										if (pLaunchData->m_fOnStateChange)
											pLaunchData->m_fOnStateChange(NMib::NPlatform::fg_FormatErrno("open (elevate stdin pipe)", errno), 0.0);
										pLaunchData->m_bFailedStateChange = true;
										return;
									}
								}
								else
								{									
									NMib::NStr::CStr Errors;
									if (!pLaunchData->m_pLaunchContext->f_RedirectStdInWrite(Pipe, Errors))
									{
										if (!pLaunchData->m_bFailedStateChange)
										{
											if (pLaunchData->m_fOnStateChange)
												pLaunchData->m_fOnStateChange(Errors, 0.0);
											pLaunchData->m_bFailedStateChange = true;
											return;
										}
									}
								}
							}
							
							if (pLaunchData->m_fOnStateChange)
								pLaunchData->m_fOnStateChange(pLaunchData->m_pLaunchedProcess, 0.0);
							
							if (!pLaunchData->m_QueuedStdOut.f_IsEmpty() && pLaunchData->m_fOnOutput)
							{
								pLaunchData->m_fOnOutput(_OutputType, pLaunchData->m_QueuedStdOut);
								pLaunchData->m_QueuedStdOut.f_Clear();
							}

							if (pLaunchData->m_fOnOutput)
							{
								for (auto iOutput = pLaunchData->m_LaunchOutputs.f_GetIterator(); iOutput; ++iOutput)
								{
									pLaunchData->m_fOnOutput(iOutput->m_OutputType, iOutput->m_Output);
								}
							}
							
							return ;
						}
						else
						{
							if (Line.f_StartsWith("gksu-run"))
								pLaunchData->m_SavedError.f_Clear();
							else
							{
								pLaunchData->m_SavedError += Line;
								pLaunchData->m_SavedError += "\n";
							}
						}
					}
				}
			}
		}
	;
	
	return true;
}

bool NMib::NProcess::NPlatform::fg_Linux_Process_RegisterAtStartup(NMib::NStr::CStr const& _ExePath, NMib::NStr::CStr const &_Params, NMib::NStr::CStr const& _Name)
{
	NMib::NStr::CStr XdgConfigHomeDir = NSys::fg_Process_GetEnvironmentVariable(NStr::CStr("XDG_CONFIG_HOME"));
	if (XdgConfigHomeDir.f_IsEmpty())
		XdgConfigHomeDir = NMib::NFile::CFile::fs_AppendPath(NSys::NFile::fg_GetUserHomeDirectory(), ".config");
	NMib::NStr::CStr AutoStartFile = NMib::NFile::CFile::fs_AppendPath(XdgConfigHomeDir, "autostart/" + _Name + ".desktop");
	
	try
	{
		NStr::CStr Content =
			"[Desktop Entry]\n"
			"Name=" + _Name + "\n"
			"Exec=" + _ExePath + " " + _Params + "\n"
			"Type=Application\n"
			"X-GNOME-Autostart-Delay=30\n"
			"Icon=\n"
			"Comment=\n"
			"NoDisplay=true;\n";
		
		NMib::NFile::CFile::fs_CreateDirectory(NMib::NFile::CFile::fs_GetPath(AutoStartFile));
		NMib::NFile::CFile::fs_WriteStringToFile(AutoStartFile, Content, false);
	}
	catch (NMib::NFile::CExceptionFile const&)
	{
		return false;
	}
	return true;
}

bool NMib::NProcess::NPlatform::fg_Linux_Process_DeRegisterAtStartup(NMib::NStr::CStr const& _ExePath, NMib::NStr::CStr const &_Params, NMib::NStr::CStr const& _Name)
{
	NMib::NStr::CStr XdgConfigHomeDir = NSys::fg_Process_GetEnvironmentVariable(NStr::CStr("XDG_CONFIG_HOME"));
	if (XdgConfigHomeDir.f_IsEmpty())
		XdgConfigHomeDir = NMib::NFile::CFile::fs_AppendPath(NSys::NFile::fg_GetUserHomeDirectory(), ".config");
	NMib::NStr::CStr AutoStartFile = NMib::NFile::CFile::fs_AppendPath(XdgConfigHomeDir, "autostart/" + _Name + ".desktop");
	
	if (NMib::NFile::CFile::fs_FileExists(AutoStartFile))
	{
		try
		{
			NMib::NFile::CFile::fs_DeleteFile(AutoStartFile);
		}
		catch (NMib::NFile::CExceptionFile const&)
		{
			return false;
		}
	}
	return true;
}

namespace NMib
{
	namespace NSys
	{
		extern const ch8* g_LinuxProgramIdentifier;
	}
}

bool NMib::NProcess::NPlatform::fg_Linux_RegisterURLHandler(NMib::NStr::CStr const &_Protocol, NMib::NStr::CStr const& _ExePath, NMib::NStr::CStr const &_Params)
{
	NMib::NStr::CStr DesktopFile = NStr::CStr::CFormat("{}_{}") << NMib::NSys::g_LinuxProgramIdentifier << NMib::NDataProcessing::fg_GetHashedUuidString(_ExePath, NMib::NDataProcessing::CUniversallyUniqueIdentifier("{4860363c-8bf6-4cb8-a22c-2b7b71c18264}"));
	NMib::NStr::CStr MimeAppsFile = NMib::NFile::CFile::fs_GetUserHomeDirectory() + "/.local/share/applications/mimeapps.list";
	
	if (NMib::NFile::CFile::fs_FileExists(MimeAppsFile))
	{
		using namespace NMib::NDesktopIntegration;
		
		CDesktopFileParser Parser(NStr::CStr(), MimeAppsFile);
		
		NMib::NContainer::TCVector<CDesktopFileParser::CDesktopGroup> lDesktopGroups;
		Parser.f_Parse(lDesktopGroups);
		
		bint bSetAddedAssociations = false;
		bint bSetDefaultApplications = false;
		
		for (CDesktopFileParser::CDesktopGroup &Group : lDesktopGroups)
		{
			if (Group.m_Name == "Added Associations" || Group.m_Name == "Default Applications")
			{
				Group.m_KeyValueMap[_Protocol] = DesktopFile + ".desktop";
				bSetAddedAssociations |= Group.m_Name == "Added Associations";
				bSetDefaultApplications |= Group.m_Name == "Default Applications";
			}
		}
		
		if (!bSetAddedAssociations && !bSetDefaultApplications)
		{
			CDesktopFileParser::CDesktopGroup& Group = lDesktopGroups.f_Insert();
			Group.m_Name = "Added Associations";
			Group.m_KeyValueMap[_Protocol] = DesktopFile + ".desktop";
		}
		
		Parser.f_Write(lDesktopGroups, false);
	}
	else
	{
		try
		{
			NMib::NFile::CFile::fs_CreateDirectory(NMib::NFile::CFile::fs_GetPath(MimeAppsFile));
			NMib::NStr::CStr Contents = NMib::NStr::CStr::CFormat("[Added Associations]\n{}={}.desktop\n") << _Protocol << DesktopFile;
			NMib::NFile::CFile::fs_WriteStringToFile((NMib::NStr::CStr)MimeAppsFile, Contents, false);
			return true;
		}
		catch (NMib::NFile::CExceptionFile const&)
		{
			return false;
		}
	}
	
	return false;
}

bool NMib::NProcess::NPlatform::fg_Linux_DeRegisterURLHandler(NMib::NStr::CStr const &_Protocol)
{
	NMib::NStr::CStr MimeAppsFile = NMib::NFile::CFile::fs_GetUserHomeDirectory() + "/.local/share/applications/mimeapps.list";
	
	if (NMib::NFile::CFile::fs_FileExists(MimeAppsFile))
	{
		using namespace NMib::NDesktopIntegration;
		
		CDesktopFileParser Parser(NStr::CStr(), MimeAppsFile);
		
		NMib::NContainer::TCVector<CDesktopFileParser::CDesktopGroup> lDesktopGroups;
		Parser.f_Parse(lDesktopGroups);
		
		for (CDesktopFileParser::CDesktopGroup &Group : lDesktopGroups)
		{
			if (Group.m_Name == "Added Associations" || Group.m_Name == "Default Applications")
			{
				if (Group.m_KeyValueMap.f_Exists(_Protocol))
					Group.m_KeyValueMap.f_Remove(_Protocol);
			}
		}
					
		Parser.f_Write(lDesktopGroups, false);
	}
	
	return true;
}

bool NMib::NProcess::NPlatform::fg_Linux_LaunchDocumentOrURL(NMib::NProcess::CProcessLaunchParams &_Params, NMib::NStr::CStr &_Program, NMib::NStr::CStr &_oErrors)
{
	if 
		(
			_Params.m_Elevation != NProcess::EProcessLaunchElevation_DeElevate
			&& 
			(
				NMib::NProcess::CProcessLaunch::fs_GetElevation() == NMib::NProcess::EProcessElevation_IsRoot 
				|| NMib::NProcess::CProcessLaunch::fs_GetElevation() == NMib::NProcess::EProcessElevation_IsElevated
			)
		)
	{
		_oErrors = "Launching a document or URL elevated is not supported on Linux.";
		return false;
	}

	if (_Params.m_Elevation == NProcess::EProcessLaunchElevation_Elevate)
	{
		_oErrors = "Launching a document or URL elevated is not supported on Linux.";
		return false;
	}

	_Program = "xdg-open";				

	NStr::CStr Document = _Params.m_Target;
	
	if (_Params.m_LaunchType == NProcess::EProcessLaunchType_URL)
	{
		// Make sure the scheme is added if not already present
		bint bSchemeExist = false;
		aint Pos = Document.f_Find(":");
		if (Pos > -1)
		{
			NStr::CStr Scheme = Document.f_Extract(0, Pos);
			const ch8 * pParse = Scheme;
			bSchemeExist = true;
			while (*pParse)
			{
				// Check for allowed RFC 1738 scheme chars
				if (NStr::fg_CharIsAlphabetical(*pParse)
					|| NStr::fg_CharIsNumber(*pParse)
					|| *pParse == '+'
					|| *pParse == '.'
					|| *pParse == '-')
				{
					++pParse;
				}
				else
				{
					bSchemeExist = false;
					break;
				}
				
			}
		}
		if (!bSchemeExist)
			Document = (NStr::CStr::CFormat("http://{}") << Document).f_GetStr();
	}
	else if (_Params.m_Operation == "open folder")
	{
		Document = Document.f_Replace("select,", "");
		Document = Document.f_Replace("\"", "");
		
		if (NMib::NFile::CFile::fs_FileExists(Document, NMib::NFile::EFileAttrib_File))
			Document = NMib::NFile::CFile::fs_GetPath(Document);
	}
	
	_Params.m_Parameters = Document.f_EscapeStr();
	_Params.m_bAllowExecutableLocate = true;
	_Params.m_LaunchType = NProcess::EProcessLaunchType_Executable;
	_Params.m_bSeparateStdErr = true;
	
	auto SavedOnStateChange = fg_Move(_Params.m_fOnStateChange);
	auto SavedOnOutput = fg_Move(_Params.m_fOnOutput);
	
	struct CLaunchOutput
	{
		EProcessLaunchOutputType m_OutputType;
		NMib::NStr::CStr m_Output;
	};
	
	struct CLaunchData
	{
		CLaunchData()
			: m_pLaunchedProcess(nullptr)
		{
		}
		
		NContainer::TCVector<CLaunchOutput> m_LaunchOutputs;
		
		NMib::NThread::CMutual m_Lock;
		
		void * m_pLaunchedProcess;
	};
	
	NPtr::TCSharedPointer<CLaunchData> pLaunchData = fg_Construct();
	
	
	_Params.m_fOnStateChange =
		[SavedOnStateChange, SavedOnOutput, pLaunchData](NMib::NProcess::CProcessLaunchStateChangeVariant const &_State, fp64 _TimeSinceStart)
		{
			
			switch (_State.f_GetTypeID())
			{
			case NMib::NProcess::EProcessLaunchState_Exited:
				{
					DMibLock(pLaunchData->m_Lock);
					
					if (_State.f_Get<NMib::NProcess::EProcessLaunchState_Exited>() != 0)
					{
						NStr::CStr ErrorStr;
						
						for(auto iOutput = pLaunchData->m_LaunchOutputs.f_GetIterator(); iOutput; ++iOutput)
						{
							if (iOutput->m_OutputType != EProcessLaunchOutputType_StdOut)
								ErrorStr += iOutput->m_Output;
						}
						
						if (SavedOnStateChange)
							SavedOnStateChange(ErrorStr, _TimeSinceStart);
					}
					else
					{
						if (SavedOnStateChange)
							SavedOnStateChange((void*)pLaunchData->m_pLaunchedProcess, _TimeSinceStart);
						
						if (SavedOnOutput)
						{
							for(auto iOutput = pLaunchData->m_LaunchOutputs.f_GetIterator(); iOutput; ++iOutput)
							{
								SavedOnOutput(iOutput->m_OutputType, iOutput->m_Output);
							}
						}

						if (SavedOnStateChange)
							SavedOnStateChange(_State, _TimeSinceStart);
					}
				}
				break;
					
			case NMib::NProcess::EProcessLaunchState_Launched:
				{
					DMibLock(pLaunchData->m_Lock);
					
					pLaunchData->m_pLaunchedProcess = _State.f_Get<NMib::NProcess::EProcessLaunchState_Launched>();
				}
				break;
					
			case NMib::NProcess::EProcessLaunchState_LaunchFailed:
				{
					if (SavedOnStateChange)
						SavedOnStateChange(_State, _TimeSinceStart);
				}
				break;
			}
		}
	;
	
	_Params.m_fOnOutput =
		[pLaunchData](EProcessLaunchOutputType _OutputType, NMib::NStr::CStr const &_Output)
		{
			
			DMibLock(pLaunchData->m_Lock);
			
			auto & Output = pLaunchData->m_LaunchOutputs.f_Insert();
			
			Output.m_OutputType = _OutputType;
			Output.m_Output = _Output;
		}
	;
	
	return true;
}
