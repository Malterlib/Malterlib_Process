// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Core/Core>
#include <Mib/Storage/Variant>
#include "Malterlib_Process_StdInActor.h"
#include <Mib/Concurrency/ActorSubscription>

#define DMibProcessLogStdInPrompt 0

namespace NMib::NProcess
{
	using namespace NMib::NStr;
	struct CStdInActor::CInternal : public NConcurrency::CActorInternal
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
			void f_CheckOutputPrompt()
			{
				if (m_bOutputPrompt)
					return;

				m_bOutputPrompt = true;
				if (!m_Params.m_Prompt.f_IsEmpty())
					fs_StdOutput(m_StdOutActor, m_Params.m_Prompt);
			}

			void f_ChangeInsertPos(umint _iInsertPos)
			{
				if (_iInsertPos > m_iInsertPos)
					fs_StdOutput(m_StdOutActor, "\x1B[{}C"_f << (_iInsertPos - m_iInsertPos));
				else if (m_iInsertPos > _iInsertPos)
					fs_StdOutput(m_StdOutActor, "\x1B[{}D"_f << (m_iInsertPos - _iInsertPos));
				else
					fs_StdOutput(m_StdOutActor, "\x7");

				m_iRenderInsertPos = m_iInsertPos = _iInsertPos;
			}

			void f_Redraw(umint _OldLen, umint _NewInsertPos)
			{
				umint ExtraChars = 0;
				if (_OldLen > umint(m_Result.f_GetLen()))
					ExtraChars = _OldLen - m_Result.f_GetLen();

				umint InsertPosFromEnd = (m_Result.f_GetLen() + ExtraChars) - _NewInsertPos;

				NStr::CStr OutputString;

				if (m_iRenderInsertPos)
					OutputString += "\x1B[{}D"_f << m_iRenderInsertPos;

				if (m_Params.m_bPassword)
					OutputString += "{sf*,sj*}"_f << "" << m_Result.f_GetLen();
				else
					OutputString += m_Result;

				if (ExtraChars)
					OutputString += "{sf ,sj*}"_f << "" << ExtraChars;

				if (InsertPosFromEnd)
					OutputString += "\x1B[{}D"_f << InsertPosFromEnd;

				fs_StdOutput(m_StdOutActor, OutputString);

				m_iRenderInsertPos = _NewInsertPos;
			}

			CPrompt(CStdInReaderPromptParams _Params, NConcurrency::TCActor<NConcurrency::CSeparateThreadActor> const &_StdOutActor)
				: m_StdOutActor(_StdOutActor)
				, m_Params(_Params)
			{
			}

			~CPrompt()
			{
				if (m_bOutputPrompt && !m_Params.m_Prompt.f_IsEmpty())
					fs_StdOutput(m_StdOutActor, DMibNewLine);
			}

			NConcurrency::TCActor<NConcurrency::CSeparateThreadActor> m_StdOutActor;

			CStdInReaderPromptParams m_Params;
			NStr::CUStrSecure m_Result;
			umint m_iInsertPos = 0;
			umint m_iRenderInsertPos = 0;
			umint m_iDrawInsertPos = 0;
			bool m_bOutputPrompt = false;
			bool m_bInsert = true;
		};

		using CReadEntryInfo = NStorage::TCStreamableVariant
			<
				EReadEntry
				, NStorage::TCMember<void, EReadEntry_None>
				, NStorage::TCMember<CLine, EReadEntry_Line>
				, NStorage::TCMember<CPrompt, EReadEntry_Prompt>
			>
		;

		struct CReadEntry
		{
			NConcurrency::TCPromise<NStr::CStrIO> m_Promise;
			CReadEntryInfo m_EntryInfo;
		};

		struct CReadEntryBinary
		{
			NConcurrency::TCPromise<NContainer::CIOByteVector> m_Promise;
			NContainer::CIOByteVector m_Buffer;
		};

		struct CBufferedStdIn
		{
			EStdInReaderOutputType m_Type;
			NStr::CStrIO m_Input;
		};

		struct CBufferedStdInBinary
		{
			EStdInReaderOutputType m_Type;
			NContainer::CIOByteVector m_Input;
			NStr::CStr m_Error;
		};

		CInternal(CStdInActor *_pThis)
			: m_pThis(_pThis)
		{
		}

		// The output actor echoes prompts, so it only exists once a prompt is registered; a
		// process that only reads its input never starts its thread
		NConcurrency::TCActor<NConcurrency::CSeparateThreadActor> const &f_GetStdOutActor()
		{
			if (!m_StdOutActor)
				m_StdOutActor = fg_Construct(fg_Construct(), "StdIn output actor");

			return m_StdOutActor;
		}

		void f_RegisterForRead();
		void f_RegisterForReadBinary();
		void f_HandleBufferedStdIn();
		void f_HandleBufferedStdInBinary();
		static void fs_StdOutput(NConcurrency::TCActor<NConcurrency::CSeparateThreadActor> const &_StdOutActor, NStr::CStr const &_String);

		CStdInActor *m_pThis;
		NContainer::TCSet<NStorage::TCSharedPointer<CSubscription, NStorage::CSupportWeakTag>> m_Subscriptions;

		NStorage::TCSharedPointer<CSubscription, NStorage::CSupportWeakTag> m_pReadSubscription;
		NContainer::TCLinkedList<CReadEntry> m_ReadEntries;
		NContainer::TCLinkedList<CBufferedStdIn> m_BufferedStdIn;

		NStorage::TCSharedPointer<CSubscription, NStorage::CSupportWeakTag> m_pReadSubscriptionBinary;
		NContainer::TCLinkedList<CReadEntryBinary> m_ReadEntriesBinary;
		NContainer::TCLinkedList<CBufferedStdInBinary> m_BufferedStdInBinary;

		NConcurrency::TCActor<NConcurrency::CSeparateThreadActor> m_StdOutActor;
	};

	CStdInActor::CStdInActor()
		: mp_pInternal(fg_Construct(this))
	{
	}

	CStdInActor::~CStdInActor()
	{
	}

	NConcurrency::TCFuture<void> CStdInActor::fp_Destroy()
	{
		f_AbortReads();
		auto &Internal = *mp_pInternal;

		co_await fg_TempCopy(Internal.m_StdOutActor).f_Destroy();
		co_return {};
	}

	void CStdInActor::f_AbortReads()
	{
		auto &Internal = *mp_pInternal;
		for (auto &Entry : Internal.m_ReadEntries)
			Entry.m_Promise.f_SetException(DMibErrorInstance("Abandoned"));
		for (auto &Entry : Internal.m_ReadEntriesBinary)
			Entry.m_Promise.f_SetException(DMibErrorInstance("Abandoned"));

		Internal.m_ReadEntries.f_Clear();
		Internal.m_ReadEntriesBinary.f_Clear();
		Internal.m_pReadSubscription.f_Clear();
		Internal.m_pReadSubscriptionBinary.f_Clear();
		Internal.m_BufferedStdIn.f_Clear();
		Internal.m_BufferedStdInBinary.f_Clear();
	}

	void CStdInActor::CInternal::f_HandleBufferedStdIn()
	{
		while (true)
		{
			if (m_ReadEntries.f_IsEmpty())
			{
				if (!m_Subscriptions.f_IsEmpty())
					m_BufferedStdIn.f_Clear();
				return;
			}

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
				ReadEntry.m_Promise.f_SetException(DMibErrorInstance(BufferEntry.m_Input));
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
						ReadEntry.m_Promise.f_SetResult(NStr::fg_GetStrLineSep(BufferEntry.m_Input));
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

					umint QueuedInsertPos = 0;
					smint QueuedOldLen = -1;

					auto fQueueRedraw = [&](umint _OldLen, umint _InsertPos)
						{
							QueuedInsertPos = _InsertPos;
							if (QueuedOldLen < 0)
								QueuedOldLen = _OldLen;
							Prompt.m_iInsertPos = _InsertPos;
						}
					;
					auto fFlushRedraw = [&]()
						{
							if (QueuedOldLen == -1)
								return;
							Prompt.f_Redraw(QueuedOldLen, QueuedInsertPos);
							QueuedOldLen = -1;
						}
					;

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
											umint nPlaces = Parameters.f_ToInt(umint(1));
											auto NewPos = fg_Clamp(Prompt.m_iInsertPos + nPlaces, 0u, umint(Prompt.m_Result.f_GetLen()));
											fFlushRedraw();
											Prompt.f_ChangeInsertPos(NewPos);
											break;
										}
									case 'D':
										{
											smint nPlaces = Parameters.f_ToInt(smint(1));
											auto NewPos = fg_Clamp(smint(Prompt.m_iInsertPos) - nPlaces, smint(0), smint(Prompt.m_Result.f_GetLen()));
											fFlushRedraw();
											Prompt.f_ChangeInsertPos(NewPos);
											break;
										}
									case '~':
										{
											if (Parameters == "3")
											{
												if (Prompt.m_iInsertPos < umint(Prompt.m_Result.f_GetLen()))
												{
													umint OldLen = Prompt.m_Result.f_GetLen();
													Prompt.m_Result = Prompt.m_Result.f_Delete(Prompt.m_iInsertPos, 1);
													fQueueRedraw(OldLen, Prompt.m_iInsertPos);
												}
												else
													fs_StdOutput(m_StdOutActor, "\x7");
											}
											else if (Parameters == "2")
												Prompt.m_bInsert = !Prompt.m_bInsert;
#if DMibProcessLogStdInPrompt
											else
												DMibConOut("Unknown ~: {}\n", Parameters);
#endif
											break;
										}
#if DMibProcessLogStdInPrompt
									default:
										{
											NStr::CUStr End;
											End.f_AddChar(Command);
											DMibConOut("Param: {}   Inter: {}   End: {}\n", Parameters, Intermediate, End);
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
									DMibConOut("Invalid Escape: {}\n", *iEscape);
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
										DMibConOut("Ignore OSC: {}\n", Command);
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
								DMibConOut("Ignore Esc: {}\n", End);
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
								umint OldLen = Prompt.m_Result.f_GetLen();
								Prompt.m_Result = Prompt.m_Result.f_Delete(Prompt.m_Result.f_GetLen() - 1, 1);
								fQueueRedraw(OldLen, Prompt.m_iInsertPos - 1);
							}
							else
								fs_StdOutput(m_StdOutActor, "\x7");
						}
						else if (NewChar == 1) // Home
						{
							++iUTFChar;
							umint OldLen = Prompt.m_Result.f_GetLen();
							fQueueRedraw(OldLen, 0);
						}
						else if (NewChar == 5) // End
						{
							++iUTFChar;
							umint OldLen = Prompt.m_Result.f_GetLen();
							fQueueRedraw(OldLen, OldLen);
						}
						else if (NewChar >= 32)
						{
							++iUTFChar;
							ch32 Temp[2] = {NewChar, 0};

							umint OldLen = Prompt.m_Result.f_GetLen();
							if (Prompt.m_bInsert)
							{
								if (Prompt.m_iInsertPos == OldLen)
									Prompt.m_Result.f_AddChar(NewChar);
								else
									fg_StrInsert(Prompt.m_Result, Prompt.m_iInsertPos, Temp);
							}
							else
							{
								if (Prompt.m_iInsertPos < OldLen)
									Prompt.m_Result[Prompt.m_iInsertPos] = NewChar;
								else
									Prompt.m_Result.f_AddChar(NewChar);
							}
							fQueueRedraw(OldLen, Prompt.m_iInsertPos + 1);
						}
						else
						{
#if DMibProcessLogStdInPrompt
							DMibConOut("Ignore: {nfh}\n", NewChar);
#endif
							++iUTFChar; // Ignore all other control characters
						}
					}

					fFlushRedraw();

					BufferEntry.m_Input = BufferEntry.m_Input.f_Extract(iUTFChar.f_GetLastWholeCodePointPos());
					if (BufferEntry.m_Input.f_IsEmpty())
						m_BufferedStdIn.f_Remove(BufferEntry);

					if (bCompleted)
					{
						ReadEntry.m_Promise.f_SetResult(Prompt.m_Result);
						m_ReadEntries.f_Remove(ReadEntry);
						continue;
					}
					else if (bAborted)
					{
						ReadEntry.m_Promise.f_SetException(DMibErrorInstance("Aborted"));
						m_ReadEntries.f_Remove(ReadEntry);
						continue;
					}
					else
					{
						for (auto &BufferEntry : m_BufferedStdIn)
						{
							if (BufferEntry.m_Type == EStdInReaderOutputType_GeneralError)
							{
								ReadEntry.m_Promise.f_SetException(DMibErrorInstance(BufferEntry.m_Input));
								m_ReadEntries.f_Remove(ReadEntry);
								while (m_BufferedStdIn.f_Pop().m_Type != EStdInReaderOutputType_GeneralError)
									;
								break;
							}
						}
					}
					break;
				}
			default:
				DMibNeverGetHere;
			}

			break;
		}
	}

	void CStdInActor::CInternal::fs_StdOutput(NConcurrency::TCActor<NConcurrency::CSeparateThreadActor> const &_StdOutActor, NStr::CStr const &_String)
	{
		_StdOutActor.f_Bind<NMib::NCommandLine::fg_MalterlibConErrOut>(_String).f_DiscardResult();
	}

	void CStdInActor::CInternal::f_HandleBufferedStdInBinary()
	{
		while (true)
		{
			if (m_ReadEntriesBinary.f_IsEmpty())
				return;

			if (m_BufferedStdInBinary.f_IsEmpty())
				return;

			auto &ReadEntry = m_ReadEntriesBinary.f_GetFirst();
			auto &BufferEntry = m_BufferedStdInBinary.f_GetFirst();

			if (BufferEntry.m_Type == EStdInReaderOutputType_GeneralError)
			{
				ReadEntry.m_Promise.f_SetException(DMibErrorInstance(BufferEntry.m_Error));
				m_ReadEntriesBinary.f_Remove(ReadEntry);
				continue;
			}
			if (BufferEntry.m_Type == EStdInReaderOutputType_EndOfFile)
			{
				ReadEntry.m_Promise.f_SetResult(fg_Move(ReadEntry.m_Buffer));
				m_ReadEntriesBinary.f_Remove(ReadEntry);
				continue;
			}

			ReadEntry.m_Buffer.f_Insert(BufferEntry.m_Input.f_GetArray(), BufferEntry.m_Input.f_GetLen());
			m_BufferedStdInBinary.f_Remove(BufferEntry);
		}
	}

	void CStdInActor::CInternal::f_RegisterForRead()
	{
		if (m_pReadSubscription)
			return;

		// Registered on a pool thread's loop, so the input arrives on a thread that exists anyway
		// instead of waking the shared poller, which would be started for this alone
		NConcurrency::CIoLoopCreateScope IoLoopScope(NConcurrency::fg_ConcurrencyManager().f_PickIoLoopBinding(NConcurrency::EPriority_Normal));

		m_pReadSubscription = fg_Construct
			(
				CStdInReaderParams::fs_Create
				(
					[=, this, pThisWeak = fg_ThisActor(m_pThis).f_Weak()](EStdInReaderOutputType _Type, NStr::CStrIO const &_Input)
					{
						auto pThis = pThisWeak.f_Lock();
						if (!pThis)
							return;

						NConcurrency::g_Dispatch(pThis) / [=, this]
							{
								auto HandleInput = g_OnScopeExit / [&, this]
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
							> NConcurrency::g_DiscardResult
						;

					}
					, EStdInReaderFlag_None
				)
			)
		;
	}

	void CStdInActor::CInternal::f_RegisterForReadBinary()
	{
		if (m_pReadSubscriptionBinary)
			return;

		NConcurrency::CIoLoopCreateScope IoLoopScope(NConcurrency::fg_ConcurrencyManager().f_PickIoLoopBinding(NConcurrency::EPriority_Normal));

		m_pReadSubscriptionBinary = fg_Construct
			(
				CStdInReaderParams::fs_CreateBinary
				(
					[=, this, pThisWeak = fg_ThisActor(m_pThis).f_Weak()](EStdInReaderOutputType _Type, NContainer::CIOByteVector const &_Input, CStr const &_Error)
					{
						auto pThis = pThisWeak.f_Lock();
						if (!pThis)
							return;

						NConcurrency::g_Dispatch(pThis) / [=, this]
							{
								auto HandleInput = g_OnScopeExit / [&, this]
									{
										f_HandleBufferedStdInBinary();
									}
								;
								if (m_BufferedStdInBinary.f_IsEmpty())
								{
									m_BufferedStdInBinary.f_Insert({_Type, _Input, _Error});
									return;
								}
								auto &Last = m_BufferedStdInBinary.f_GetLast();
								if (Last.m_Type == _Type)
								{
									if (_Type == EStdInReaderOutputType_StdIn)
										Last.m_Input.f_Insert(_Input.f_GetArray(), _Input.f_GetLen());
									else
										Last.m_Error += _Error;
									return;
								}
								m_BufferedStdInBinary.f_Insert({_Type, _Input, _Error});
							}
							> NConcurrency::g_DiscardResult
						;

					}
					, EStdInReaderFlag_None
				)
			)
		;
	}

	NConcurrency::TCFuture<NConcurrency::CActorSubscription> CStdInActor::f_RegisterForInput(FOnInput _fOnInput, EStdInReaderFlag _Flags, umint _MaxSize)
	{
		auto &Internal = *mp_pInternal;
		try
		{
			NStorage::TCSharedPointer<CInternal::CSubscription, NStorage::CSupportWeakTag> pSubscription = fg_Construct
				(
					CStdInReaderParams::fs_Create
					(
						[fOnInput = fg_Move(_fOnInput), _MaxSize](EStdInReaderOutputType _Type, NStr::CStrIO const &_Input)
						{
							umint InputLen = _Input.f_GetLen();
							if (InputLen > _MaxSize)
							{
								NMisc::fg_ChunkRange
									(
										umint(0)
										, InputLen
										, _MaxSize
										, [&](umint _Start, umint _Len)
										{
											fOnInput.f_CallDiscard(_Type, _Input.f_Extract(_Start, _Len));
										}
									)
								;
							}
							else
								fOnInput.f_CallDiscard(_Type, _Input);
						}
						, _Flags
					)
				)
			;

			Internal.m_Subscriptions[pSubscription];

			co_return NConcurrency::g_ActorSubscription / [this, pSubscriptionWeak = pSubscription.f_Weak()]
				{
					auto &Internal = *mp_pInternal;
					Internal.m_Subscriptions.f_Remove(pSubscriptionWeak);
				}
			;
		}
		catch (NException::CException const &)
		{
			co_return NException::fg_CurrentException();
		}
	}

	NConcurrency::TCFuture<NConcurrency::CActorSubscription> CStdInActor::f_RegisterForInputBinary(FOnBinaryInput _fOnInput, EStdInReaderFlag _Flags, umint _MaxSize)
	{
		auto &Internal = *mp_pInternal;
		try
		{
			NStorage::TCSharedPointer<CInternal::CSubscription, NStorage::CSupportWeakTag> pSubscription = fg_Construct
				(
					CStdInReaderParams::fs_CreateBinary
					(
						[fOnInput = fg_Move(_fOnInput), _MaxSize](EStdInReaderOutputType _Type, NContainer::CIOByteVector const &_Input, CStr const &_Error)
						{
							umint InputLen = _Input.f_GetLen();
							if (InputLen > _MaxSize)
							{
								NMisc::fg_ChunkRange
									(
										umint(0)
										, InputLen
										, _MaxSize
										, [&](umint _Start, umint _Len)
										{
											NContainer::CIOByteVector ToSend;
											ToSend.f_Insert(_Input.f_GetArray() + _Start, _Len);

											fOnInput.f_CallDiscard(_Type, fg_Move(ToSend), _Error);
										}
									)
								;
							}
							else
								fOnInput.f_CallDiscard(_Type, _Input, _Error);
						}
						, _Flags
					)
				)
			;

			Internal.m_Subscriptions[pSubscription];

			co_return NConcurrency::g_ActorSubscription / [this, pSubscriptionWeak = pSubscription.f_Weak()]
				{
					auto &Internal = *mp_pInternal;
					Internal.m_Subscriptions.f_Remove(pSubscriptionWeak);
				}
			;
		}
		catch (NException::CException const &)
		{
			co_return NException::fg_CurrentException();
		}
	}

	NConcurrency::TCFuture<NStr::CStrIO> CStdInActor::f_ReadLine()
	{
		auto &Internal = *mp_pInternal;

		auto &Entry = Internal.m_ReadEntries.f_Insert();
		auto Future = Entry.m_Promise.f_Future();

		Entry.m_EntryInfo = CInternal::CLine{};

		Internal.f_RegisterForRead();
		Internal.f_HandleBufferedStdIn();

		co_return co_await fg_Move(Future);
	}

	NConcurrency::TCFuture<NContainer::CIOByteVector> CStdInActor::f_ReadBinary()
	{
		auto &Internal = *mp_pInternal;

		auto &Entry = Internal.m_ReadEntriesBinary.f_Insert();
		auto Future = Entry.m_Promise.f_Future();

		Internal.f_RegisterForReadBinary();
		Internal.f_HandleBufferedStdInBinary();

		co_return co_await fg_Move(Future);
	}

	NConcurrency::TCFuture<NStr::CStrIO> CStdInActor::f_ReadPrompt(CStdInReaderPromptParams _Params)
	{
		auto &Internal = *mp_pInternal;

		auto &Entry = Internal.m_ReadEntries.f_Insert();
		auto Future = Entry.m_Promise.f_Future();

		Entry.m_EntryInfo = CInternal::CPrompt{_Params, Internal.f_GetStdOutActor()};

		Internal.f_RegisterForRead();
		Internal.f_HandleBufferedStdIn();

		co_return co_await fg_Move(Future);
	}
}
