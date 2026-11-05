// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <Mib/Core/Core>

namespace NMib::NProcess
{
	enum EStdInReaderFlag
	{
		EStdInReaderFlag_None                   = 0
		, EStdInReaderFlag_Exclusive            = DMibBit(0) // Set to gurantee exclusive input, otherwise several CStdInReader can be created and all will receive input
		, EStdInReaderFlag_VirtualTerminalInput = DMibBit(2) // Set to receive terminal input as virtual terminal sequences on Windows consoles
		, EStdInReaderFlag_LineInput            = DMibBit(3) // Set to keep the line editing, echo and signals of a POSIX terminal while every reader sets it
	};

	enum EStdInReaderOutputType
	{
		EStdInReaderOutputType_StdIn
		, EStdInReaderOutputType_GeneralError
		, EStdInReaderOutputType_EndOfFile
	};

	struct CStdInReaderParams
	{
		CStdInReaderParams();
		CStdInReaderParams(CStdInReaderParams const &_From) = delete;
		CStdInReaderParams(CStdInReaderParams &&_From);

		CStdInReaderParams &operator = (CStdInReaderParams const &_From) = delete;
		CStdInReaderParams &operator = (CStdInReaderParams &&_From);

		static auto fs_Create
			(
				NFunction::TCFunctionMovable<void (EStdInReaderOutputType _Type, NStr::CStrIO const &_Input)> &&_fOnReceiveInput
				, EStdInReaderFlag _Flags = EStdInReaderFlag_None
				, NFunction::TCFunctionMovable<void (NFunction::TCFunctionMovable<void ()> &&_fFunctor)> &&_fDispatcher = {}
			)
			-> CStdInReaderParams
		;
		static auto fs_CreateBinary
			(
				NFunction::TCFunctionMovable<void (EStdInReaderOutputType _Type, NContainer::CIOByteVector const &_Input, NStr::CStr const &_Error)> &&_fOnReceiveInput
				, EStdInReaderFlag _Flags = EStdInReaderFlag_None
				, NFunction::TCFunctionMovable<void (NFunction::TCFunctionMovable<void ()> &&_fFunctor)> &&_fDispatcher = {}
			)
			-> CStdInReaderParams
		;

		EStdInReaderFlag m_Flags;
		NFunction::TCFunctionMovable<void (EStdInReaderOutputType _Type, NStr::CStrIO const &_Input)> m_fOnReceiveInput;
		NFunction::TCFunctionMovable<void (EStdInReaderOutputType _Type, NContainer::CIOByteVector const &_Input, NStr::CStr const &_Error)> m_fOnReceiveBinaryInput;
		NFunction::TCFunctionMovable<void (NFunction::TCFunctionMovable<void ()> &&_fFunctor)> m_fDispatcher;
	};

	struct CStdInReaderPromptParams
	{
		template <typename tf_CStream>
		void f_Stream(tf_CStream &_Stream);

		NStr::CStr m_Prompt;
		bool m_bPassword = false;
	};

	struct CStdInReader
	{
		CStdInReader(CStdInReaderParams &&_Params);
		~CStdInReader();
		CStdInReader(CStdInReader &&_Other);

	private:
		CStdInReader(CStdInReader const &_Other) = delete;
		CStdInReader &operator = (CStdInReader const &_Other) = delete;

		void fp_CheckOpen() const;

		void *mp_pStdInReader;
	};

	COnScopeExitShared fg_StdInReader_RegisterScreenChange(NFunction::TCFunction<void (NSys::CConsoleProperties const &_ConsoleProperties)> &&_fOnScreenChange);

	struct CBlockingStdInReader
	{
		using CPromptParams = CStdInReaderPromptParams;

		CBlockingStdInReader();
		~CBlockingStdInReader();
		NStr::CStr f_ReadLine();
		NStr::CStr f_TryReadLine();
		bool f_ReadPrompt(CPromptParams const &_Params, NStr::CStr &_Result);

	private:
		NThread::CMutual mp_Lock;
		NStr::CStr mp_Buffer;
		NStr::CStr mp_Errors;
		NThread::CEventAutoReset mp_Event;
		NStorage::TCUniquePointer<CStdInReader> mp_pStdInReader;
	};
}

#include "Malterlib_Process_StdIn.hpp"

#ifndef DMibPNoShortCuts
using namespace NMib::NProcess;
#endif
