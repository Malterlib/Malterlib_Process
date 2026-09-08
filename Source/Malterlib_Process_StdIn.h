// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <Mib/Core/Core>

namespace NMib::NProcess
{
	enum EStdInReaderFlag
	{
		EStdInReaderFlag_None = 0
		, EStdInReaderFlag_Exclusive = DMibBit(0) // Set to gurantee exclusive input, otherwise several CStdInReader can be created and all will receive input
		, EStdInReaderFlag_VirtualTerminalInput = DMibBit(1) // Set to receive terminal input as virtual terminal sequences on Windows consoles
	};

	enum EStdInReaderOutputType
	{
		EStdInReaderOutputType_StdIn
		, EStdInReaderOutputType_GeneralError
		, EStdInReaderOutputType_EndOfFile
	};

	struct CStdInReaderParams
	{
	public:

	public:
		CStdInReaderParams();
		CStdInReaderParams(CStdInReaderParams const &_From) = delete;
		CStdInReaderParams(CStdInReaderParams &&_From);

		CStdInReaderParams &operator =(CStdInReaderParams const &_From) = delete;
		CStdInReaderParams &operator =(CStdInReaderParams &&_From);

		EStdInReaderFlag m_Flags;
		NFunction::TCFunctionMovable<void (EStdInReaderOutputType _Type, NStr::CStrIO const &_Input)> m_fOnReceiveInput;
		NFunction::TCFunctionMovable<void (EStdInReaderOutputType _Type, NContainer::CIOByteVector const &_Input, NStr::CStr const &_Error)> m_fOnReceiveBinaryInput;
		NFunction::TCFunctionMovable<void (NFunction::TCFunctionMovable<void ()> &&_Functor)> m_fDispatcher;

		static CStdInReaderParams fs_Create
			(
				NFunction::TCFunctionMovable<void (EStdInReaderOutputType _Type, NStr::CStrIO const &_Input)> &&_fOnReceiveInput
				, EStdInReaderFlag _Flags = EStdInReaderFlag_None
				, NFunction::TCFunctionMovable<void (NFunction::TCFunctionMovable<void ()> &&_Functor)> &&_fDispatcher = {}
			)
		;
		static CStdInReaderParams fs_CreateBinary
			(
				NFunction::TCFunctionMovable<void (EStdInReaderOutputType _Type, NContainer::CIOByteVector const &_Input, NStr::CStr const &_Error)> &&_fOnReceiveInput
				, EStdInReaderFlag _Flags = EStdInReaderFlag_None
				, NFunction::TCFunctionMovable<void (NFunction::TCFunctionMovable<void ()> &&_Functor)> &&_fDispatcher = {}
			)
		;
	};
	struct CStdInReaderPromptParams
	{
	public:
		template <typename tf_CStream>
		void f_Stream(tf_CStream &_Stream);

		NStr::CStr m_Prompt;
		bool m_bPassword = false;
	};

	class CStdInReader
	{
		CStdInReader(CStdInReader const &) = delete;
		CStdInReader &operator = (CStdInReader const &) = delete;

		void *m_pStdInReader;
		void fp_CheckOpen() const;
	public:
		CStdInReader(CStdInReaderParams &&_Params);
		~CStdInReader();
		CStdInReader(CStdInReader &&_Other);
	};

	// Screen size changes, learned of through the console input the standard input reader owns.
	// Windows only in practice: there a resize is a record in the same queue keystrokes arrive in,
	// so the reader that consumes the queue is the only thing that can see it, and this fails
	// unless a CStdInReader of a console standard input is open. POSIX has SIGWINCH for this and
	// refuses the call; callers there register with NSys::fg_System_RegisterForSignal
	COnScopeExitShared fg_StdInReader_RegisterScreenChange(NFunction::TCFunction<void (NSys::CConsoleProperties const &_ConsoleProperties)> &&_fOnScreenChange);

	class CBlockingStdInReader
	{
	public:
		using CPromptParams = CStdInReaderPromptParams;

	private:
		NThread::CMutual m_Lock;
		NStr::CStr m_Buffer;
		NStr::CStr m_Errors;
		NThread::CEventAutoReset m_Event;
		NStorage::TCUniquePointer<CStdInReader> m_pStdInReader;

	public:

		CBlockingStdInReader();
		~CBlockingStdInReader();
		NStr::CStr f_ReadLine();
		NStr::CStr f_TryReadLine();
		bool f_ReadPrompt(CPromptParams const &_Params, NStr::CStr &_Result);
	};
}

#include "Malterlib_Process_StdIn.hpp"

#ifndef DMibPNoShortCuts
	using namespace NMib::NProcess;
#endif
