// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>

namespace NMib::NProcess
{
	enum EStdInReaderFlag
	{
		EStdInReaderFlag_None = 0
		, EStdInReaderFlag_Exclusive = DMibBit(0) // Set to gurantee exclusive input, otherwise several CStdInReader can be created and all will receive input
		, EStdInReaderFlag_ForcePolling = DMibBit(1) // Set to force polling behaviour. Mainly for unit testing to make sure that XP polling mode works
	};

	enum EStdInReaderOutputType
	{
		EStdInReaderOutputType_StdIn
		, EStdInReaderOutputType_GeneralError
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
		NFunction::TCFunctionMovable<void (EStdInReaderOutputType _Type, NStr::CStr const &_Input)> m_fOnReceiveInput;
		NFunction::TCFunctionMovable<void (NFunction::TCFunctionMovable<void ()> &&_Functor)> m_fDispatcher;

		static CStdInReaderParams fs_Create
			(
				NFunction::TCFunctionMovable<void (EStdInReaderOutputType _Type, NStr::CStr const &_Input)> &&_fOnReceiveInput
				, EStdInReaderFlag _Flags = EStdInReaderFlag_None
				, NFunction::TCFunctionMovable<void (NFunction::TCFunctionMovable<void ()> &&_Functor)> &&_fDispatcher = {} 
			)
		;
	};
	struct CStdInReaderPromptParams
	{
	public:
		CStdInReaderPromptParams()
			: m_bPassword(false)
		{
		}

		template <typename tf_CStream>
		void f_Stream(tf_CStream &_Stream);

		NStr::CStr m_Prompt;
		bool m_bPassword;
	};

	class CStdInReader
	{
		DMibClassNoCopyAllowed(CStdInReader);
		void *m_pStdInReader;
		void fp_CheckOpen() const;
	public:
		CStdInReader(CStdInReaderParams &&_Params);
		~CStdInReader();
		CStdInReader(CStdInReader &&_Other);
	};
	
	class CBlockingStdInReader
	{
	public:
		using CPromptParams = CStdInReaderPromptParams;

	private:
		NThread::CMutual m_Lock;
		NStr::CStr m_Buffer;
		NStr::CStr m_Errors;
		NThread::CEventAutoReset m_Event;
		NPtr::TCUniquePointer<CStdInReader> m_pStdInReader;

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

