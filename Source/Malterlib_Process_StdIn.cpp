// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Process/StdIn>
#include "Malterlib_Process_Platform.h"

namespace NMib::NProcess
{
	CBlockingStdInReader::CBlockingStdInReader()
	{
		CStdInReaderParams Params
			= CStdInReaderParams::fs_Create
			(
				[this](EStdInReaderOutputType _Type, NMib::NStr::CStr const &_Input)
				{
					DMibLock(m_Lock);
					if (_Type == EStdInReaderOutputType_StdIn)
						m_Buffer += _Input;
					else
						m_Errors += _Input;
					m_Event.f_Signal();
				}
				, EStdInReaderFlag_Exclusive
			)
		;

		m_pStdInReader = fg_Construct(fg_Move(Params));
	}
	
	CBlockingStdInReader::~CBlockingStdInReader()
	{
		m_pStdInReader.f_Clear();
	}
	
	NStr::CStr CBlockingStdInReader::f_TryReadLine()
	{
		{
			DMibLock(m_Lock);
			if (!m_Errors.f_IsEmpty())
				DMibError(m_Errors);
			
			aint iNewLine = m_Buffer.f_FindChar('\n');
			if (iNewLine >= 0)
				return NStr::fg_GetStrLineSep(m_Buffer);
		}
		return NStr::CStr();
	}
	
	NStr::CStr CBlockingStdInReader::f_ReadLine()
	{
		while (true)
		{
			{
				DMibLock(m_Lock);
				
				if (!m_Errors.f_IsEmpty())
					DMibError(m_Errors);
				
				aint iNewLine = m_Buffer.f_FindChar('\n');
				if (iNewLine >= 0)
					return NStr::fg_GetStrLineSep(m_Buffer);
			}
			m_Event.f_Wait();
		}
		return NStr::CStr();
	}

	bool CBlockingStdInReader::f_ReadPrompt(CPromptParams const &_Params, NStr::CStr &_Result)
	{
		bool bAborted = false;
		bool bCompleted = false;
		NStr::CUStr Result;
		
		if (!_Params.m_Prompt.f_IsEmpty())
			DMibConOutRaw(_Params.m_Prompt);

		while (!bCompleted && !bAborted)
		{
			{
				DMibLock(m_Lock);

				if (!m_Errors.f_IsEmpty())
					DMibError(m_Errors);

				if (!m_Buffer.f_IsEmpty())
				{
					auto iUTFChar = m_Buffer.f_GetUnicodeIterator();
					
					while (iUTFChar && iUTFChar.f_IsWholeCodePoint())
					{
						auto NewChar = *iUTFChar;
						++iUTFChar;
					
						if (NewChar == '\x1B' || NewChar == '\x3')		// Escape or end of text (ctrl break)
						{
							bAborted = true;
							break;
						}
						else if (NewChar == '\r' || NewChar == '\n' || NewChar == '\x4')	// Newline, return or end of transmission
						{
							bCompleted = true;
							if (NewChar == '\r' && iUTFChar && *iUTFChar == '\n')
								++iUTFChar;
							break;
						}
						else if (NewChar == '\x8' || NewChar == '\x7F')	// Backspace (or del)
						{
							if (Result.f_GetLen())
							{
								Result = Result.f_Delete(Result.f_GetLen() - 1, 1);
								if (!_Params.m_Prompt.f_IsEmpty())
									DMibConOutRaw("\x8 \x8");			// Go back one character, overwrite it, go back again
							}
						}
						else if (NewChar >= 32)		// Ignore all other control characters
						{
							Result.f_AddChar(NewChar);
							NStr::CUStr Char;
							Char.f_AddChar(NewChar);
							if (!_Params.m_bPassword)
								DMibConOutRaw(Char);
							else
								DMibConOutRaw("*");
						}
					}
					
					m_Buffer = m_Buffer.f_Extract(iUTFChar.f_GetLastWholeCodePointPos());
				}
			}
			if (!bCompleted && !bAborted)
				m_Event.f_Wait();
		}

		if (!_Params.m_Prompt.f_IsEmpty())
			DMibConOutRaw(DMibNewLine);

		if (bAborted)
			return false;

		_Result = Result;

		return true;
	}

	CStdInReader::CStdInReader(CStdInReaderParams &&_Params)
		: m_pStdInReader(nullptr)
	{
		m_pStdInReader = NPlatform::fg_Process_StdInReader_Open(fg_Move(_Params));
	}
	
	CStdInReader::~CStdInReader()
	{
		if (m_pStdInReader)
			NPlatform::fg_Process_StdInReader_Close(m_pStdInReader);
	}
	
	CStdInReader::CStdInReader(CStdInReader &&_Other)
		: m_pStdInReader(_Other.m_pStdInReader)
	{
		_Other.m_pStdInReader = nullptr;
	}

	CStdInReaderParams CStdInReaderParams::fs_Create
		(
			NFunction::TCFunctionMovable<void (EStdInReaderOutputType _Type, NStr::CStrIO const &_Input)> &&_fOnReceiveInput
			, EStdInReaderFlag _Flags
			, NFunction::TCFunctionMovable<void (NFunction::TCFunctionMovable<void ()> &&_Functor)> &&_fDispatcher
		)
	{
		CStdInReaderParams Ret;
		Ret.m_fOnReceiveInput = fg_Move(_fOnReceiveInput);
		Ret.m_Flags = _Flags;
		Ret.m_fDispatcher = fg_Move(_fDispatcher);
		return Ret;
	}

	CStdInReaderParams CStdInReaderParams::fs_CreateBinary
		(
			NFunction::TCFunctionMovable<void (EStdInReaderOutputType _Type, NContainer::CIOByteVector const &_Input, NStr::CStr const &_Error)> &&_fOnReceiveInput
			, EStdInReaderFlag _Flags
			, NFunction::TCFunctionMovable<void (NFunction::TCFunctionMovable<void ()> &&_Functor)> &&_fDispatcher
		)
	{
		CStdInReaderParams Ret;
		Ret.m_fOnReceiveBinaryInput = fg_Move(_fOnReceiveInput);
		Ret.m_Flags = _Flags;
		Ret.m_fDispatcher = fg_Move(_fDispatcher);
		return Ret;
	}

	CStdInReaderParams::CStdInReaderParams() = default;
	CStdInReaderParams::CStdInReaderParams(CStdInReaderParams &&_From) = default;
	CStdInReaderParams &CStdInReaderParams::operator =(CStdInReaderParams &&_From) = default;
}
