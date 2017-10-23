// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Storage/Variant>
#include "Malterlib_Process_StdInActor.h"
#include <Mib/Concurrency/ActorSubscription>

#define DMibProcessLogStdInPrompt 0

namespace NMib::NProcess
{
	using namespace NMib::NStr;
	struct CStdInActor::CInternal
	{
		struct CSubscription
		{
			CSubscription(CStdInReaderParams &&_Params)
				: m_StdInReader(fg_Move(_Params))
			{
			}
			
			CStdInReader m_StdInReader;
		};

		enum EReadEntry
		{
			EReadEntry_None
			, EReadEntry_Line
			, EReadEntry_Prompt
		};

		struct CLine
		{
		};

		struct CPrompt
		{
			CStdInReaderPromptParams m_Params;
			NStr::CUStrSecure m_Result;
			mint m_iInsertPos = 0;
			bool m_bOutputPrompt = false;
			bool m_bInsert = true;

			void f_CheckOutputPrompt()
			{
				if (m_bOutputPrompt)
					return;

				m_bOutputPrompt = true;
				if (!m_Params.m_Prompt.f_IsEmpty())
					DMibConErrOutRaw(m_Params.m_Prompt);
			}

			void f_ChangeInsertPos(mint _iInsertPos)
			{
				if (_iInsertPos > m_iInsertPos)
					DMibConErrOut2("\x1B[{}C", _iInsertPos - m_iInsertPos);
				else if (m_iInsertPos > _iInsertPos)
					DMibConErrOut2("\x1B[{}D", m_iInsertPos - _iInsertPos);
				else
					DMibConErrOut2("\x7");

				m_iInsertPos = _iInsertPos;
			}

			void f_Redraw(mint _OldLen, mint _NewInsertPos)
			{
				mint ExtraChars = 0;
				if (_OldLen > mint(m_Result.f_GetLen()))
					ExtraChars = _OldLen - m_Result.f_GetLen();

				mint InsertPosFromEnd = (m_Result.f_GetLen() + ExtraChars) - _NewInsertPos;

				NStr::CStr OutputString;

				if (m_iInsertPos)
					OutputString += "\x1B[{}D"_f << m_iInsertPos;

				if (m_Params.m_bPassword)
					OutputString += "{sf*,sj*}"_f << "" << m_Result.f_GetLen();
				else
					OutputString += m_Result;

				if (ExtraChars)
					OutputString += "{sf ,sj*}"_f << "" << ExtraChars;

				if (InsertPosFromEnd)
					OutputString += "\x1B[{}D"_f << InsertPosFromEnd;

				DMibConErrOutRaw(OutputString);

				m_iInsertPos = _NewInsertPos;
			}

			~CPrompt()
			{
				if (m_bOutputPrompt && !m_Params.m_Prompt.f_IsEmpty())
					DMibConErrOutRaw(DMibNewLine);
			}
		};

		using CReadEntryInfo = NContainer::TCStreamableVariant<EReadEntry, void, EReadEntry_None, CLine, EReadEntry_Line, CPrompt, EReadEntry_Prompt>;

		struct CReadEntry
		{
			CReadEntryInfo m_EntryInfo;
			NConcurrency::TCContinuation<NStr::CStrSecure> m_Continuation;
		};

		struct CBufferedStdIn
		{
			EStdInReaderOutputType m_Type;
			NStr::CStrSecure m_Input;
		};

		CInternal(CStdInActor *_pThis)
			: m_pThis(_pThis)
		{
		}

		void f_RegisterForRead();
		void f_HandleBufferedStdIn();

		CStdInActor *m_pThis;
		NContainer::TCSet<NPtr::TCSharedPointer<CSubscription, NPtr::CSupportWeakTag>> m_Subscriptions;

		NPtr::TCSharedPointer<CSubscription, NPtr::CSupportWeakTag> m_pReadSubscription;
		NContainer::TCLinkedList<CReadEntry> m_ReadEntries;
		NContainer::TCLinkedList<CBufferedStdIn> m_BufferedStdIn;
	};
	
	CStdInActor::CStdInActor()
		: mp_pInternal(fg_Construct(this))
	{
	}
	
	CStdInActor::~CStdInActor()
	{
	}

	NConcurrency::TCContinuation<void> CStdInActor::fp_Destory()
	{
		f_AbortReads();
		return fg_Explicit();
	}

	void CStdInActor::f_AbortReads()
	{
		auto &Internal = *mp_pInternal;
		for (auto &Entry : Internal.m_ReadEntries)
			Entry.m_Continuation.f_SetException(DMibErrorInstance("Abandoned"));

		Internal.m_ReadEntries.f_Clear();
		Internal.m_pReadSubscription.f_Clear();
		Internal.m_BufferedStdIn.f_Clear();
	}

	void CStdInActor::CInternal::f_HandleBufferedStdIn()
	{
		while (true)
		{
			if (m_ReadEntries.f_IsEmpty())
				return;

			auto &ReadEntry = m_ReadEntries.f_GetFirst();

			if (m_BufferedStdIn.f_IsEmpty())
			{
				if (ReadEntry.m_EntryInfo.f_GetTypeID() == EReadEntry_Prompt)
					ReadEntry.m_EntryInfo.f_Get<EReadEntry_Prompt>().f_CheckOutputPrompt();
				return;
			}

			auto &BufferEntry = m_BufferedStdIn.f_GetFirst();

			if (BufferEntry.m_Type == EStdInReaderOutputType_GeneralError)
			{
				ReadEntry.m_Continuation.f_SetException(DMibErrorInstance(BufferEntry.m_Input));
				m_ReadEntries.f_Remove(ReadEntry);
				continue;
			}

			switch (ReadEntry.m_EntryInfo.f_GetTypeID())
			{
			case EReadEntry_Line:
				{
					aint iNewLine = BufferEntry.m_Input.f_FindChar('\n');
					if (iNewLine >= 0)
					{
						ReadEntry.m_Continuation.f_SetResult(NStr::fg_GetStrLineSep(BufferEntry.m_Input));
						m_ReadEntries.f_Remove(ReadEntry);
						continue;
					}
					break;
				}
			case EReadEntry_Prompt:
				{
					auto &Prompt = ReadEntry.m_EntryInfo.f_Get<EReadEntry_Prompt>();

					Prompt.f_CheckOutputPrompt();

					auto iUTFChar = BufferEntry.m_Input.f_GetUnicodeIterator();

					bool bAborted = false;
					bool bCompleted = false;

					while (iUTFChar && iUTFChar.f_IsWholeCodePoint())
					{
						auto NewChar = *iUTFChar;

						if (NewChar == '\x1B')
						{
							// Escape sequence
							auto iEscape = iUTFChar;
							++iEscape;

							// CSI - Control Sequence Introducer
							if (!iEscape)
								break;
							else if (*iEscape == '[')
							{
								++iEscape;
								NStr::CUStr Parameters;
								while (iEscape && *iEscape >= 0x30 == *iEscape <= 0x3F)
								{
									Parameters.f_AddChar(*iEscape);
									++iEscape;
								}
								NStr::CUStr Intermediate;
								while (iEscape && *iEscape >= 0x20 == *iEscape <= 0x2F)
								{
									Intermediate.f_AddChar(*iEscape);
									++iEscape;
								}
								if (!iEscape)
									break;
								else if (*iEscape >= 0x40 && *iEscape <= 0x7E)
								{
									ch32 Command = *iEscape;
									++iEscape;
									iUTFChar = iEscape;

									switch (Command)
									{
									case 'C':
										{
											mint nPlaces = Parameters.f_ToInt(mint(1));
											auto NewPos = fg_Clamp(Prompt.m_iInsertPos + nPlaces, 0, mint(Prompt.m_Result.f_GetLen()));
											Prompt.f_ChangeInsertPos(NewPos);
											break;
										}
									case 'D':
										{
											smint nPlaces = Parameters.f_ToInt(smint(1));
											auto NewPos = fg_Clamp(smint(Prompt.m_iInsertPos) - nPlaces, smint(0), smint(Prompt.m_Result.f_GetLen()));
											Prompt.f_ChangeInsertPos(NewPos);
											break;
										}
									case '~':
										{
											if (Parameters == "3")
											{
												if (Prompt.m_iInsertPos < mint(Prompt.m_Result.f_GetLen()))
												{
													mint OldLen = Prompt.m_Result.f_GetLen();
													Prompt.m_Result = Prompt.m_Result.f_Delete(Prompt.m_iInsertPos, 1);
													Prompt.f_Redraw(OldLen, Prompt.m_iInsertPos);
												}
												else
													DMibConErrOut2("\x7");
											}
											else if (Parameters == "2")
												Prompt.m_bInsert = !Prompt.m_bInsert;
#if DMibProcessLogStdInPrompt
											else
												DMibConOut2("Unknown ~: {}\n", Parameters);
#endif
											break;
										}
#if DMibProcessLogStdInPrompt
									default:
										{
											NStr::CUStr End;
											End.f_AddChar(Command);
											DMibConOut2("Param: {}   Inter: {}   End: {}\n", Parameters, Intermediate, End);
											break;
										}
#endif
									}
								}
								else if (*iEscape == 0)
									break;
								else
								{
#if DMibProcessLogStdInPrompt
									DMibConOut2("Ivalid Escape: {}\n", *iEscape);
#endif
									// Invalid CSI?
									++iEscape;
									iUTFChar = iEscape;
								}
							}
							else if (*iEscape == ']' || *iEscape == 'X' || *iEscape == '^' || *iEscape == '_')
							{
								bool bIsOSC = *iEscape == ']';
								++iEscape;
								ch32 Prev = 0;
#if DMibProcessLogStdInPrompt
								NStr::CUStr Command;
#endif
								while (true)
								{
									if (!iEscape)
										break;
									if (bIsOSC && *iEscape == 7)
									{
										++iEscape;
										iUTFChar = iEscape;
										break;
									}
									if (Prev == '\x1B' && *iEscape == '\\')
									{
										++iEscape;
										iUTFChar = iEscape;
#if DMibProcessLogStdInPrompt
										DMibConOut2("Ignore OSC: {}\n", Command);
#endif
										break;
									}
#if DMibProcessLogStdInPrompt
									Command.f_AddChar(*iEscape);
#endif
									Prev = *iEscape;
									++iEscape;
								}
							}
							else
							{
#if DMibProcessLogStdInPrompt
								NStr::CUStr End;
								End.f_AddChar(*iEscape);
								DMibConOut2("Ignore Esc: {}\n", End);
#endif
								// Ignore other escape escape sequences
								++iEscape;
								iUTFChar = iEscape;
							}
						}
						else if (NewChar == '\x3')		// End of text (ctrl break)
						{
							++iUTFChar;
							bAborted = true;
							continue;
						}
						else if (NewChar == '\r' || NewChar == '\n' || NewChar == '\x4')	// Newline, return or end of transmission
						{
							++iUTFChar;
							bCompleted = true;
							if (NewChar == '\r' && iUTFChar && *iUTFChar == '\n')
								++iUTFChar;
							break;
						}
						else if (NewChar == '\x8' || NewChar == '\x7F')	// Backspace (or del)
						{
							++iUTFChar;
							if (Prompt.m_iInsertPos > 0)
							{
								mint OldLen = Prompt.m_Result.f_GetLen();
								Prompt.m_Result = Prompt.m_Result.f_Delete(Prompt.m_Result.f_GetLen() - 1, 1);
								Prompt.f_Redraw(OldLen, Prompt.m_iInsertPos - 1);
							}
							else
								DMibConErrOut2("\x7");
						}
						else if (NewChar >= 32)
						{
							++iUTFChar;
							ch32 Temp[2] = {NewChar, 0};

							mint OldLen = Prompt.m_Result.f_GetLen();
							if (Prompt.m_bInsert)
								fg_StrInsert(Prompt.m_Result, Prompt.m_iInsertPos, Temp);
							else
							{
								if (Prompt.m_iInsertPos < OldLen)
									Prompt.m_Result[Prompt.m_iInsertPos] = NewChar;
								else
									Prompt.m_Result.f_AddChar(NewChar);
							}
							Prompt.f_Redraw(OldLen, Prompt.m_iInsertPos + 1);
						}
						else
						{
#if DMibProcessLogStdInPrompt
							DMibConOut2("Ignore: {nfh}\n", NewChar);
#endif
							++iUTFChar; // Ignore all other control characters
						}
					}

					BufferEntry.m_Input = BufferEntry.m_Input.f_Extract(iUTFChar.f_GetLastWholeCodePointPos());

					if (bCompleted)
					{
						ReadEntry.m_Continuation.f_SetResult(Prompt.m_Result);
						m_ReadEntries.f_Remove(ReadEntry);
						continue;
					}
					else if (bAborted)
					{
						ReadEntry.m_Continuation.f_SetException(DMibErrorInstance("Aborted"));
						m_ReadEntries.f_Remove(ReadEntry);
						continue;
					}


					break;
				}
			default:
				DMibNeverGetHere;
			}

			break;
		}
	}

	void CStdInActor::CInternal::f_RegisterForRead()
	{
		if (m_pReadSubscription)
			return;

		m_pReadSubscription = fg_Construct
			(
				CStdInReaderParams::fs_Create
				(
					[=, pThisWeak = fg_ThisActor(m_pThis).f_Weak()](EStdInReaderOutputType _Type, NStr::CStrSecure const &_Input)
					{
						auto pThis = pThisWeak.f_Lock();
						if (!pThis)
							return;

						NConcurrency::g_Dispatch(pThis) > [=]
							{
								auto HandleInput = g_OnScopeExit > [&]
									{
										f_HandleBufferedStdIn();
									}
								;
								if (m_BufferedStdIn.f_IsEmpty())
								{
									m_BufferedStdIn.f_Insert({_Type, _Input});
									return;
								}
								auto &Last = m_BufferedStdIn.f_GetLast();
								if (Last.m_Type == _Type)
								{
									Last.m_Input += _Input;
									return;
								}
								m_BufferedStdIn.f_Insert({_Type, _Input});
							}
							> NConcurrency::fg_DiscardResult()
						;

					}
					, EStdInReaderFlag_None
				)
			)
		;
	}

	NConcurrency::TCContinuation<NConcurrency::CActorSubscription> CStdInActor::f_RegisterForInput(FOnInput &&_fOnInput, EStdInReaderFlag _Flags)
	{
		auto &Internal = *mp_pInternal;
		NConcurrency::TCContinuation<NConcurrency::CActorSubscription> Continuation;
		try
		{
			NPtr::TCSharedPointer<CInternal::CSubscription, NPtr::CSupportWeakTag> pSubscription = fg_Construct
				(
					CStdInReaderParams::fs_Create
					(
						[fOnInput = fg_Move(_fOnInput)](EStdInReaderOutputType _Type, NStr::CStrSecure const &_Input)
						{
							fOnInput(_Type, _Input) > NConcurrency::fg_DiscardResult();
						}
						, _Flags
					)
				)
			;
			
			Internal.m_Subscriptions[pSubscription];
			
			Continuation.f_SetResult
				(
					NConcurrency::g_ActorSubscription > [this, pSubscriptionWeak = pSubscription.f_Weak()]
					{
						auto &Internal = *mp_pInternal;
						Internal.m_Subscriptions.f_Remove(pSubscriptionWeak);
					}
				)
			;
		}
		catch (NException::CException const &)
		{
			Continuation.f_SetCurrentException();
		}
		return Continuation;
	}

	NConcurrency::TCContinuation<NStr::CStrSecure> CStdInActor::f_ReadLine()
	{
		auto &Internal = *mp_pInternal;

		auto &Entry = Internal.m_ReadEntries.f_Insert();
		Entry.m_EntryInfo = CInternal::CLine{};
		Internal.f_RegisterForRead();
		Internal.f_HandleBufferedStdIn();

		return Entry.m_Continuation;
	}

	NConcurrency::TCContinuation<NStr::CStrSecure> CStdInActor::f_ReadPrompt(CStdInReaderPromptParams const &_Params)
	{
		auto &Internal = *mp_pInternal;

		auto &Entry = Internal.m_ReadEntries.f_Insert();
		Entry.m_EntryInfo = CInternal::CPrompt{_Params};
		Internal.f_RegisterForRead();
		Internal.f_HandleBufferedStdIn();

		return Entry.m_Continuation;
	}
}
