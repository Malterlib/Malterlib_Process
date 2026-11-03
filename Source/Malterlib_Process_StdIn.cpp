// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Core/Core>
#include <Mib/Process/StdIn>
#include "Malterlib_Process_Platform.h"

namespace NMib::NProcess
{
	CBlockingStdInReader::CBlockingStdInReader()
	{
		CStdInReaderParams Params = CStdInReaderParams::fs_Create
			(
				[this](EStdInReaderOutputType _Type, NMib::NStr::CStr const &_Input)
				{
					DMibLock(mp_Lock);
					if (_Type == EStdInReaderOutputType_StdIn)
						mp_Buffer += _Input;
					else
						mp_Errors += _Input;
					mp_Event.f_Signal();
				}
				, EStdInReaderFlag_Exclusive
			)
		;

		mp_pStdInReader = fg_Construct(fg_Move(Params));
	}

	CBlockingStdInReader::~CBlockingStdInReader()
	{
		mp_pStdInReader.f_Clear();
	}

	NStr::CStr CBlockingStdInReader::f_TryReadLine()
	{
		{
			DMibLock(mp_Lock);
			if (!mp_Errors.f_IsEmpty())
				DMibError(mp_Errors);

			aint iNewLine = mp_Buffer.f_FindChar('\n');
			if (iNewLine >= 0)
				return NStr::fg_GetStrLineSep(mp_Buffer);
		}
		return NStr::CStr();
	}

	NStr::CStr CBlockingStdInReader::f_ReadLine()
	{
		while (true)
		{
			{
				DMibLock(mp_Lock);

				if (!mp_Errors.f_IsEmpty())
					DMibError(mp_Errors);

				aint iNewLine = mp_Buffer.f_FindChar('\n');
				if (iNewLine >= 0)
					return NStr::fg_GetStrLineSep(mp_Buffer);
			}
			mp_Event.f_Wait();
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
				DMibLock(mp_Lock);

				if (!mp_Errors.f_IsEmpty())
					DMibError(mp_Errors);

				if (!mp_Buffer.f_IsEmpty())
				{
					auto iUTFChar = mp_Buffer.f_GetUnicodeIterator();

					while (iUTFChar && iUTFChar.f_IsWholeCodePoint())
					{
						auto NewChar = *iUTFChar;
						++iUTFChar;

						if (NewChar == '\x1B' || NewChar == '\x3') // Escape or end of text (ctrl break)
						{
							bAborted = true;
							break;
						}
						else if (NewChar == '\r' || NewChar == '\n' || NewChar == '\x4') // Newline, return or end of transmission
						{
							bCompleted = true;
							if (NewChar == '\r' && iUTFChar && *iUTFChar == '\n')
								++iUTFChar;
							break;
						}
						else if (NewChar == '\x8' || NewChar == '\x7F') // Backspace (or del)
						{
							if (Result.f_GetLen())
							{
								Result = Result.f_Delete(Result.f_GetLen() - 1, 1);
								if (!_Params.m_Prompt.f_IsEmpty())
									DMibConOutRaw("\x8 \x8"); // Go back one character, overwrite it, go back again
							}
						}
						else if (NewChar >= 32) // Ignore all other control characters
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

					mp_Buffer = mp_Buffer.f_Extract(iUTFChar.f_GetLastWholeCodePointPos());
				}
			}
			if (!bCompleted && !bAborted)
				mp_Event.f_Wait();
		}

		if (!_Params.m_Prompt.f_IsEmpty())
			DMibConOutRaw(DMibNewLine);

		if (bAborted)
			return false;

		_Result = Result;

		return true;
	}

	// Requires an open Windows console stdin reader; callbacks run on its I/O-loop thread. Unsupported on POSIX.
	COnScopeExitShared fg_StdInReader_RegisterScreenChange(NFunction::TCFunction<void (NSys::CConsoleProperties const &_ConsoleProperties)> &&_fOnScreenChange)
	{
		return NPlatform::fg_Process_StdInReader_RegisterScreenChange(fg_Move(_fOnScreenChange));
	}

	CStdInReader::CStdInReader(CStdInReaderParams &&_Params)
		: mp_pStdInReader(nullptr)
	{
		mp_pStdInReader = NPlatform::fg_Process_StdInReader_Open(fg_Move(_Params));
	}

	CStdInReader::~CStdInReader()
	{
		if (mp_pStdInReader)
			NPlatform::fg_Process_StdInReader_Close(mp_pStdInReader);
	}

	CStdInReader::CStdInReader(CStdInReader &&_Other)
		: mp_pStdInReader(_Other.mp_pStdInReader)
	{
		_Other.mp_pStdInReader = nullptr;
	}

	auto CStdInReaderParams::fs_Create
		(
			NFunction::TCFunctionMovable<void (EStdInReaderOutputType _Type, NStr::CStrIO const &_Input)> &&_fOnReceiveInput
			, EStdInReaderFlag _Flags
			, NFunction::TCFunctionMovable<void (NFunction::TCFunctionMovable<void ()> &&_fFunctor)> &&_fDispatcher
		)
		-> CStdInReaderParams
	{
		CStdInReaderParams Ret;
		Ret.m_fOnReceiveInput = fg_Move(_fOnReceiveInput);
		Ret.m_Flags = _Flags;
		Ret.m_fDispatcher = fg_Move(_fDispatcher);
		return Ret;
	}

	auto CStdInReaderParams::fs_CreateBinary
		(
			NFunction::TCFunctionMovable<void (EStdInReaderOutputType _Type, NContainer::CIOByteVector const &_Input, NStr::CStr const &_Error)> &&_fOnReceiveInput
			, EStdInReaderFlag _Flags
			, NFunction::TCFunctionMovable<void (NFunction::TCFunctionMovable<void ()> &&_fFunctor)> &&_fDispatcher
		)
		-> CStdInReaderParams
	{
		CStdInReaderParams Ret;
		Ret.m_fOnReceiveBinaryInput = fg_Move(_fOnReceiveInput);
		Ret.m_Flags = _Flags;
		Ret.m_fDispatcher = fg_Move(_fDispatcher);
		return Ret;
	}

	CStdInReaderParams::CStdInReaderParams() = default;
	CStdInReaderParams::CStdInReaderParams(CStdInReaderParams &&_From) = default;
	CStdInReaderParams &CStdInReaderParams::operator = (CStdInReaderParams &&_From) = default;
}
