// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>

namespace NMib
{
	namespace NProcess
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
			CStdInReaderParams(CStdInReaderParams const &_From);
			CStdInReaderParams(CStdInReaderParams &&_From);

			CStdInReaderParams &operator =(CStdInReaderParams const &_From);
			CStdInReaderParams &operator =(CStdInReaderParams &&_From);

			EStdInReaderFlag m_Flags;
			NFunction::TCFunction<void (EStdInReaderOutputType _Type, NStr::CStr const &_Input)> m_fOnReceiveInput;
			NFunction::TCFunction<void (NFunction::TCFunction<void ()> const &_Functor)> m_fDispatcher;

			static CStdInReaderParams fs_Create
				(
					NFunction::TCFunction<void (EStdInReaderOutputType _Type, NStr::CStr const &_Input)> const &_fOnReceiveInput
					, EStdInReaderFlag _Flags = EStdInReaderFlag_None
					, NFunction::TCFunction<void (NFunction::TCFunction<void ()> const &_Functor)> const &_fDispatcher 
					= NFunction::TCFunction<void (NFunction::TCFunction<void ()> const &_Functor)>()
				)
			;
		};

		class CStdInReader
		{
			DMibClassNoCopyAllowed(CStdInReader);
			void *m_pStdInReader;
			void fp_CheckOpen() const;
		public:
			CStdInReader(CStdInReaderParams const &_Params);
			~CStdInReader();
			CStdInReader(CStdInReader &&_Other);
		};
		
		class CBlockingStdInReader
		{
		public:
			struct CPromptParams
			{
			public:
				CPromptParams()
					: m_bPassword(false)
				{
				}

				bool m_bPassword;
				NStr::CStr m_Prompt;
			};

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
}

#ifndef DMibPNoShortCuts
	using namespace NMib::NProcess;	
#endif

