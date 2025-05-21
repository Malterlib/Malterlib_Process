// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Process/ProxiedProcessLaunch>
#include <Mib/Concurrency/AsyncResult>
#include <Mib/Concurrency/ThreadSafeQueue>
#include <Mib/Encoding/Base64>
#include <Mib/Process/VirtualProcessLaunch>
#include <Mib/Process/StdIn>

namespace NMib::NProcess::NPrivate
{
	enum : uint32
	{
		EProtocolVersion_Current = 0x105
	};

	struct CHandleMessageVisitor;

	template <typename t_CType>
	class TCTypeToIDImp
	{
	public:
	};
	template <typename t_CType>
	class TCTypeToID
	{
	public:
		static constexpr uint32 mc_Value = TCTypeToIDImp<t_CType>::mc_Value;
	};

	template <typename t_CType>
	const uint32 TCTypeToID<t_CType>::mc_Value;

	template <uint32 t_TypeID>
	class TCIDToType
	{
	public:
		static constexpr bool mc_Defined = false;
	};

	template <uint32 t_TypeID, bool t_bDefined = TCIDToType<t_TypeID>::mc_Defined>
	struct TCEvalNumTypeID
	{
		static constexpr uint32 mc_Value = TCEvalNumTypeID<t_TypeID + 1>::mc_Value;
	};

	template <uint32 t_TypeID>
	struct TCEvalNumTypeID<t_TypeID, false>
	{
		static constexpr uint32 mc_Value = t_TypeID;
	};


	template <typename tf_CStream, typename tf_CType>
	void fg_FeedTypeID(tf_CStream &_Stream, tf_CType const &_Type)
	{
		_Stream << TCTypeToIDImp<tf_CType>::mc_Value;
	}

#define DProcessProxyProtocolType(d_Type, d_TypeID) \
	template <>\
	class TCTypeToIDImp<d_Type>\
	{\
	public:\
		static constexpr uint32 mc_Value = d_TypeID;\
	};\
	template <>\
	class TCIDToType<d_TypeID>\
	{\
	public:\
		using CType = d_Type;\
		static constexpr bool mc_Defined = true;\
		static_assert(TCIDToType<d_TypeID == 0 ? 0 : d_TypeID-1>::mc_Defined, "Gap detected in type IDs");\
	};

	class CConnect
	{
		friend struct CHandleMessageVisitor;
	public:
		CConnect()
			: m_Version(EProtocolVersion_Current)
		{
		}

		uint32 m_Version;

		struct CResponse
		{
			CResponse()
				: m_Version(EProtocolVersion_Current)
			{
			}

			uint32 m_Version;

			template <typename tf_CStream>
			void f_Feed(tf_CStream &_Stream) const
			{
				_Stream << m_Version;
			}

			template <typename tf_CStream>
			void f_Consume(tf_CStream &_Stream)
			{
				_Stream >> m_Version;
			}
		};

		template <typename tf_CStream>
		void f_Feed(tf_CStream &_Stream) const
		{
			_Stream << m_Version;
		}

		template <typename tf_CStream>
		void f_Consume(tf_CStream &_Stream)
		{
			_Stream >> m_Version;
		}

		void f_HandleMessage(CProxiedLaunchClient::CInternal *_pClient, NFunction::TCFunction<void (CResponse &_Response)> const &_OnResponse);
		void f_HandleMessage(CProxiedLaunchServer::CInternal *_pServer, NFunction::TCFunction<void (CResponse &_Response)> const &_OnResponse);
	};


	class CDisconnect
	{
		friend struct CHandleMessageVisitor;
	public:
		CDisconnect()
		{
		}

		struct CResponse
		{
			CResponse()
			{
			}

			template <typename tf_CStream>
			void f_Feed(tf_CStream &_Stream) const
			{
			}

			template <typename tf_CStream>
			void f_Consume(tf_CStream &_Stream)
			{
			}
		};

		template <typename tf_CStream>
		void f_Feed(tf_CStream &_Stream) const
		{
		}

		template <typename tf_CStream>
		void f_Consume(tf_CStream &_Stream)
		{
		}

		void f_HandleMessage(CProxiedLaunchClient::CInternal *_pClient, NFunction::TCFunction<void (CResponse &_Response)> const &_OnResponse);
		void f_HandleMessage(CProxiedLaunchServer::CInternal *_pServer, NFunction::TCFunction<void (CResponse &_Response)> const &_OnResponse);
	};

	class CProcessLaunch_Launch
	{
		CProcessLaunchParams m_Params;
		EProcessLaunchCloseFlag m_DestructFlags;
		uint64 m_LaunchID;
	public:
		CProcessLaunch_Launch(CProcessLaunchParams const &_Params, EProcessLaunchCloseFlag _DestructFlags, uint64 _LaunchID)
			: m_Params(_Params)
			, m_DestructFlags(_DestructFlags)
			, m_LaunchID(_LaunchID)
		{
		}

		CProcessLaunch_Launch()
			: m_DestructFlags(EProcessLaunchCloseFlag_None)
			, m_LaunchID(0)
		{
		}

		struct CResponse
		{
			CResponse()
			{
			}

			template <typename tf_CStream>
			void f_Feed(tf_CStream &_Stream) const
			{
			}

			template <typename tf_CStream>
			void f_Consume(tf_CStream &_Stream)
			{
			}
		};

		template <typename tf_CStream>
		void f_Feed(tf_CStream &_Stream) const
		{
			_Stream << m_Params;
			_Stream << m_DestructFlags;
			_Stream << m_LaunchID;
		}

		template <typename tf_CStream>
		void f_Consume(tf_CStream &_Stream)
		{
			_Stream >> m_Params;
			_Stream >> m_DestructFlags;
			_Stream >> m_LaunchID;
		}

		void f_HandleMessage(CProxiedLaunchClient::CInternal *_pClient, NFunction::TCFunction<void (CResponse &_Response)> const &_OnResponse);
		void f_HandleMessage(CProxiedLaunchServer::CInternal *_pServer, NFunction::TCFunction<void (CResponse &_Response)> const &_OnResponse);
	};

	class CProcessLaunch_StateChange
	{
		uint64 m_LaunchID;
		NStorage::TCStreamableVariant
			<
				EProcessLaunchState
				, NStorage::TCMember<uint8, EProcessLaunchState_Launched>
				, NStorage::TCMember<NMib::NStr::CStr, EProcessLaunchState_LaunchFailed>
				, NStorage::TCMember<uint32, EProcessLaunchState_Exited>
			>
			m_State
		;
		fp64 m_TimeSinceStart;
	public:
		CProcessLaunch_StateChange(uint64 _LaunchID, CProcessLaunchStateChangeVariant const &_State, fp64 _TimeSinceStart)
			: m_LaunchID(_LaunchID)
			, m_TimeSinceStart(_TimeSinceStart)
			, m_State("Invalid launch state")
		{
			switch (_State.f_GetTypeID())
			{
			case EProcessLaunchState_Launched:
				{
					if (_State.f_Get<EProcessLaunchState_Launched>())
						m_State = uint8(1);
					else
						m_State = uint8(0);
				}
				break;
			case EProcessLaunchState_LaunchFailed:
				{
					m_State = _State.f_Get<EProcessLaunchState_LaunchFailed>();
				}
				break;
			case EProcessLaunchState_Exited:
				{
					m_State = _State.f_Get<EProcessLaunchState_Exited>();
				}
				break;
			}
		}

		CProcessLaunch_StateChange()
			: m_LaunchID(0)
			, m_TimeSinceStart(0.0)
		{
		}

		struct CResponse
		{
			CResponse()
			{
			}

			template <typename tf_CStream>
			void f_Feed(tf_CStream &_Stream) const
			{
			}

			template <typename tf_CStream>
			void f_Consume(tf_CStream &_Stream)
			{
			}
		};

		template <typename tf_CStream>
		void f_Feed(tf_CStream &_Stream) const
		{
			_Stream << m_LaunchID;
			_Stream << m_State;
			_Stream << m_TimeSinceStart;
		}

		template <typename tf_CStream>
		void f_Consume(tf_CStream &_Stream)
		{
			_Stream >> m_LaunchID;
			_Stream >> m_State;
			_Stream >> m_TimeSinceStart;
		}

		void f_HandleMessage(CProxiedLaunchClient::CInternal *_pClient, NFunction::TCFunction<void (CResponse &_Response)> const &_OnResponse);
		void f_HandleMessage(CProxiedLaunchServer::CInternal *_pServer, NFunction::TCFunction<void (CResponse &_Response)> const &_OnResponse);
	};

	class CProcessLaunch_Output
	{
		uint64 m_LaunchID;
		EProcessLaunchOutputType m_OutputType;
		NMib::NStr::CStr m_Output;
	public:
		CProcessLaunch_Output(uint64 _LaunchID, EProcessLaunchOutputType _OutputType, NMib::NStr::CStr const &_Output)
			: m_LaunchID(_LaunchID)
			, m_OutputType(_OutputType)
			, m_Output(_Output)
		{
		}

		CProcessLaunch_Output()
			: m_LaunchID(0)
		{
		}

		struct CResponse
		{
			CResponse()
			{
			}

			template <typename tf_CStream>
			void f_Feed(tf_CStream &_Stream) const
			{
			}

			template <typename tf_CStream>
			void f_Consume(tf_CStream &_Stream)
			{
			}
		};

		template <typename tf_CStream>
		void f_Feed(tf_CStream &_Stream) const
		{
			_Stream << m_LaunchID;
			_Stream << m_OutputType;
			_Stream << m_Output;
		}

		template <typename tf_CStream>
		void f_Consume(tf_CStream &_Stream)
		{
			_Stream >> m_LaunchID;
			_Stream >> m_OutputType;
			_Stream >> m_Output;
		}

		void f_HandleMessage(CProxiedLaunchClient::CInternal *_pClient, NFunction::TCFunction<void (CResponse &_Response)> const &_OnResponse);
		void f_HandleMessage(CProxiedLaunchServer::CInternal *_pServer, NFunction::TCFunction<void (CResponse &_Response)> const &_OnResponse);
	};

	class CProcessLaunch_Input
	{
		uint64 m_LaunchID;
		NMib::NStr::CStr m_Input;
	public:
		CProcessLaunch_Input(uint64 _LaunchID, NMib::NStr::CStr const &_Input)
			: m_LaunchID(_LaunchID)
			, m_Input(_Input)
		{
		}

		CProcessLaunch_Input()
			: m_LaunchID(0)
		{
		}

		struct CResponse
		{
			CResponse()
			{
			}

			template <typename tf_CStream>
			void f_Feed(tf_CStream &_Stream) const
			{
			}

			template <typename tf_CStream>
			void f_Consume(tf_CStream &_Stream)
			{
			}
		};

		template <typename tf_CStream>
		void f_Feed(tf_CStream &_Stream) const
		{
			_Stream << m_LaunchID;
			_Stream << m_Input;
		}

		template <typename tf_CStream>
		void f_Consume(tf_CStream &_Stream)
		{
			_Stream >> m_LaunchID;
			_Stream >> m_Input;
		}

		void f_HandleMessage(CProxiedLaunchClient::CInternal *_pClient, NFunction::TCFunction<void (CResponse &_Response)> const &_OnResponse);
		void f_HandleMessage(CProxiedLaunchServer::CInternal *_pServer, NFunction::TCFunction<void (CResponse &_Response)> const &_OnResponse);
	};

	class CProcessLaunch_CloseInput
	{
		uint64 m_LaunchID;
	public:
		CProcessLaunch_CloseInput(uint64 _LaunchID)
			: m_LaunchID(_LaunchID)
		{
		}

		CProcessLaunch_CloseInput()
			: m_LaunchID(0)
		{
		}

		struct CResponse
		{
			CResponse()
			{
			}

			template <typename tf_CStream>
			void f_Feed(tf_CStream &_Stream) const
			{
			}

			template <typename tf_CStream>
			void f_Consume(tf_CStream &_Stream)
			{
			}
		};

		template <typename tf_CStream>
		void f_Feed(tf_CStream &_Stream) const
		{
			_Stream << m_LaunchID;
		}

		template <typename tf_CStream>
		void f_Consume(tf_CStream &_Stream)
		{
			_Stream >> m_LaunchID;
		}

		void f_HandleMessage(CProxiedLaunchClient::CInternal *_pClient, NFunction::TCFunction<void (CResponse &_Response)> const &_OnResponse);
		void f_HandleMessage(CProxiedLaunchServer::CInternal *_pServer, NFunction::TCFunction<void (CResponse &_Response)> const &_OnResponse);
	};

	class CProcessLaunch_Close
	{
		uint64 m_LaunchID;
		EProcessLaunchCloseFlag m_CloseFlag;
	public:
		CProcessLaunch_Close(uint64 _LaunchID, EProcessLaunchCloseFlag _CloseFlag)
			: m_LaunchID(_LaunchID)
			, m_CloseFlag(_CloseFlag)
		{
		}

		CProcessLaunch_Close()
			: m_LaunchID(0)
			, m_CloseFlag(EProcessLaunchCloseFlag_None)
		{
		}

		struct CResponse
		{
			CResponse()
			{
			}

			template <typename tf_CStream>
			void f_Feed(tf_CStream &_Stream) const
			{
			}

			template <typename tf_CStream>
			void f_Consume(tf_CStream &_Stream)
			{
			}
		};

		template <typename tf_CStream>
		void f_Feed(tf_CStream &_Stream) const
		{
			_Stream << m_LaunchID;
			_Stream << m_CloseFlag;
		}

		template <typename tf_CStream>
		void f_Consume(tf_CStream &_Stream)
		{
			_Stream >> m_LaunchID;
			_Stream >> m_CloseFlag;
		}

		void f_HandleMessage(CProxiedLaunchClient::CInternal *_pClient, NFunction::TCFunction<void (CResponse &_Response)> const &_OnResponse);
		void f_HandleMessage(CProxiedLaunchServer::CInternal *_pServer, NFunction::TCFunction<void (CResponse &_Response)> const &_OnResponse);
	};

	class CProcessLaunch_Stop
	{
		uint64 m_LaunchID;
	public:
		CProcessLaunch_Stop(uint64 _LaunchID)
			: m_LaunchID(_LaunchID)
		{
		}

		CProcessLaunch_Stop()
			: m_LaunchID(0)
		{
		}

		struct CResponse
		{
			CResponse()
			{
			}

			template <typename tf_CStream>
			void f_Feed(tf_CStream &_Stream) const
			{
			}

			template <typename tf_CStream>
			void f_Consume(tf_CStream &_Stream)
			{
			}
		};

		template <typename tf_CStream>
		void f_Feed(tf_CStream &_Stream) const
		{
			_Stream << m_LaunchID;
		}

		template <typename tf_CStream>
		void f_Consume(tf_CStream &_Stream)
		{
			_Stream >> m_LaunchID;
		}

		void f_HandleMessage(CProxiedLaunchClient::CInternal *_pClient, NFunction::TCFunction<void (CResponse &_Response)> const &_OnResponse);
		void f_HandleMessage(CProxiedLaunchServer::CInternal *_pServer, NFunction::TCFunction<void (CResponse &_Response)> const &_OnResponse);
	};

	DProcessProxyProtocolType(CConnect, 0);
	DProcessProxyProtocolType(CDisconnect, 1);
	DProcessProxyProtocolType(CProcessLaunch_Launch, 2);
	DProcessProxyProtocolType(CProcessLaunch_StateChange, 3);
	DProcessProxyProtocolType(CProcessLaunch_Output, 4);
	DProcessProxyProtocolType(CProcessLaunch_Input, 5);
	DProcessProxyProtocolType(CProcessLaunch_Close, 6);
	DProcessProxyProtocolType(CProcessLaunch_Stop, 7);
	DProcessProxyProtocolType(CProcessLaunch_CloseInput, 8);

#define DProcessProxyProtocolNumTypeID 9

#undef DProcessProxyProtocolType

	static_assert(DProcessProxyProtocolNumTypeID == TCEvalNumTypeID<0>::mc_Value, "Num type ids incorrectly defined");

	template <typename t_CVisitor>
	bool fg_VisitType(t_CVisitor &&_Visitor, uint32 _TypeID)
	{
		switch (_TypeID)
		{
		case 0: _Visitor.template operator ()<TCIDToType<0>::CType>(); break;
		case 1: _Visitor.template operator ()<TCIDToType<1>::CType>(); break;
		case 2: _Visitor.template operator ()<TCIDToType<2>::CType>(); break;
		case 3: _Visitor.template operator ()<TCIDToType<3>::CType>(); break;
		case 4: _Visitor.template operator ()<TCIDToType<4>::CType>(); break;
		case 5: _Visitor.template operator ()<TCIDToType<5>::CType>(); break;
		case 6: _Visitor.template operator ()<TCIDToType<6>::CType>(); break;
		case 7: _Visitor.template operator ()<TCIDToType<7>::CType>(); break;
		case 8: _Visitor.template operator ()<TCIDToType<8>::CType>(); break;
		return false;
		}
		return true;
#	undef DMibTemp_GenerateParam
	}
}

namespace NMib::NProcess
{
	DMibImpErrorClassImplement(CExceptionProcessProxyProtocol);

	struct CProxiedLaunchClient::CInternal
	{
		NStorage::TCUniquePointer<NMib::NProcess::CProcessLaunch> m_pServer;
		NThread::CEvent m_ServerExited;

		NStorage::TCUniquePointer<NThread::CThreadObject> m_pThreadObject;
		NThread::CMutual m_ThreadLock;
		NContainer::TCThreadSafeQueue<NFunction::TCFunction<void ()>> m_DispatchQueue;

		NAtomic::TCAtomic<smint> m_MessageID;

		NAtomic::TCAtomic<smint> m_NextSlaveLaunchID;

		uint64 f_NewLaunchID()
		{
			return ++m_NextSlaveLaunchID;
		}

		mutable NThread::CMutual m_ErrorLock;
		NStr::CStr m_StdErr;
		NStr::CStr m_Error;

		void f_SetError(NStr::CStr const &_Error)
		{
			DMibLock(m_ErrorLock);
			if (m_Error.f_IsEmpty())
			{
				m_Error = _Error;
				DMibLock(m_LaunchStatesLock);
				for (auto iLaunch = m_LaunchStates.f_GetIterator(); iLaunch; ++iLaunch)
				{
					auto pState = iLaunch->m_pState;

					if (pState->m_bIsRunning.f_Exchange(false))
					{
						CProcessLaunchStateChangeVariant Variant = _Error;
						pState->f_ReportStateChange(pState, Variant, 0.0);
						pState->m_FinishedEvent.f_SetSignaled();
					}
				}
				if (m_OnError)
					m_OnError(_Error);
			}
		}

		NStr::CStr f_GetError() const
		{
			DMibLock(m_ErrorLock);
			return m_Error;
		}

		NThread::CMutual m_ReceiveDataLock;
		NStr::CStr m_BufferedData;

		NThread::CMutual m_MessageHandlerLock;
		NContainer::TCMap<uint32, NFunction::TCFunction<void (NStream::CBinaryStreamMemoryPtr<> &_Stream)>> m_MessageHandlers;

		bool m_bLaunched = false;
		NStr::CStr m_BufferedSendData;
		NContainer::TCVector<NFunction::TCFunction<void (NStr::CStr const &_Error)>> m_BufferedSendErrors;

		NStr::CStr m_ErrorData;

		NFunction::TCFunction<void (NStr::CStr const &_Error)> m_OnError;


		void f_Dispatch(NFunction::TCFunction<void ()> const &_ToDispatch)
		{
			m_DispatchQueue.f_Push(_ToDispatch);
			{
				DMibLock(m_ThreadLock);
				m_pThreadObject->m_EventWantQuit.f_Signal();
			}
		}

		void f_SendPacket(NContainer::CByteVector const &_Data, NFunction::TCFunction<void (NStr::CStr const &_Error)> &&_fOnError)
		{
			NStr::CStr Base64Data = NEncoding::fg_Base64Encode(_Data);
			Base64Data += "\n";

			f_Dispatch
				(
					[Base64Data, this, _fOnError]()
					{
						auto Error = f_GetError();
						if (!Error.f_IsEmpty())
						{
							if (_fOnError)
								_fOnError(Error);
							return;
						}

						if (m_bLaunched)
						{
							try
							{
								m_pServer->f_SendStdIn(Base64Data);
							}
							catch (NException::CException const &_Exception)
							{
								if (_fOnError)
									_fOnError(_Exception.f_GetErrorStr());
							}
						}
						else
						{
							m_BufferedSendData += Base64Data;
							m_BufferedSendErrors.f_Insert(_fOnError);
						}
					}
				)
			;
		}

		void fp_Send(NContainer::CByteVector &&_Packet, NFunction::TCFunction<void (NStr::CStr const &_Error)> &&_fOnError)
		{
			f_SendPacket(_Packet, fg_Move(_fOnError));
		}

		void f_ReceivePacket(NStr::CStr const &_Packet);

		void f_ReceiveData(NStr::CStr const &_Data)
		{
			DMibLock(m_ReceiveDataLock);
			auto pParse = _Data.f_GetStr();

			while (*pParse)
			{
				auto pStart = pParse;
				NStr::fg_ParseToEndOfLine(pParse);
				if (pParse != pStart)
					m_BufferedData += NStr::CStr(pStart, pParse - pStart);
				if (*pParse == '\n' || *pParse == '\r')
				{
					f_ReceivePacket(m_BufferedData);
					m_BufferedData.f_Clear();
					NStr::fg_ParseEndOfLine(pParse);
				}
			}
		}

		struct CLaunchState
		{
			uint64 m_LaunchID;
			CProcessLaunchParams m_Params;
			EProcessLaunchCloseFlag m_DestructFlags;
			CProxiedLaunchClient::CInternal *m_pClient;
			NAtomic::TCAtomic<smint> m_bOpen;
			NAtomic::TCAtomic<smint> m_bIsRunning;
			mutable NThread::CMutual m_RunningTimeLock;
			NTime::CClock m_RunningTime;
			NThread::CEvent m_FinishedEvent;

			CLaunchState(CProxiedLaunchClient::CInternal *_pClient, CProcessLaunchParams  const &_Params, EProcessLaunchCloseFlag _DestructFlags);
			fp64 f_GetRunningTime() const;
			void f_ReportStateChange(NStorage::TCSharedPointer<CLaunchState> const &_pState, CProcessLaunchStateChangeVariant const &_State, fp64 _Time) const;
			void f_ReportLaunchFailed(NStorage::TCSharedPointer<CLaunchState> const &_pState, NStr::CStr const &_ErrorStr) const;
			void f_ReportOutput(NStorage::TCSharedPointer<CLaunchState> const &_pState, EProcessLaunchOutputType _OutputType, NMib::NStr::CStr const &_Output) const;
		};

		NThread::CMutual m_LaunchStatesLock;
		struct CLaunch
		{
			NStorage::TCSharedPointer<CLaunchState> m_pState;
		};
		NContainer::TCMap<uint64, CLaunch> m_LaunchStates;

		void f_AddLaunchState(uint64 _LaunchID, NStorage::TCSharedPointer<CLaunchState> const &_State)
		{
			DMibLock(m_LaunchStatesLock);
			auto &Launch = m_LaunchStates[_LaunchID];
			Launch.m_pState = _State;
		}
		void f_RemoveLaunchState(uint64 _LaunchID)
		{
			DMibLock(m_LaunchStatesLock);
			auto *pSlave = m_LaunchStates.f_FindEqual(_LaunchID);
			if (pSlave)
				m_LaunchStates.f_Remove(pSlave);
		}
		NStorage::TCSharedPointer<CLaunchState> f_GetLaunchState(uint64 _LaunchID)
		{
			DMibLock(m_LaunchStatesLock);
			auto *pState = m_LaunchStates.f_FindEqual(_LaunchID);
			if (pState)
				return pState->m_pState;
			return nullptr;
		}

		template <typename tf_CMessage>
		void f_SendMessageAsync
			(
				tf_CMessage const &_Message
				, NFunction::TCFunction<void (NConcurrency::TCAsyncResult<typename tf_CMessage::CResponse> const &_Result)> const &_Response
			)
		;

		class CVirtualProcessLaunch_Client : public CVirtualProcessLaunch
		{
			friend class CBuildServer;

			NStorage::TCSharedPointer<CLaunchState> m_pState;
		public:
			CVirtualProcessLaunch_Client(CProxiedLaunchClient::CInternal *_pClient, CProcessLaunchParams const &_Params, EProcessLaunchCloseFlag _DestructFlags);
			virtual ~CVirtualProcessLaunch_Client();
			virtual EProcessLaunchCloseFlag f_GetCloseFlags() const override;
			virtual void f_Close(EProcessLaunchCloseFlag _CloseFlags) override;
			virtual void f_StopProcess() const override;
			virtual bool f_IsOpen() const override;
			virtual bool f_IsRunning() const override;
			virtual void f_SendStdIn(NMib::NStr::CStrIO const &_Data) const override;
			virtual void f_CloseStdIn() const override;
			virtual fp64 f_GetRunningTime() const override;
		};

		CInternal(NMib::NProcess::CProcessLaunchParams const &_ServerLaunchParams, NFunction::TCFunction<void (NStr::CStr const &_Error)> const &_OnError)
			: m_OnError(_OnError)
		{
			{
				DMibLock(m_ThreadLock);
				m_pThreadObject = NThread::CThreadObject::fs_StartThread
					(
						[this](NThread::CThreadObject *_pThread) -> aint
						{
							while (_pThread->f_GetState() != NThread::EThreadState_EventWantQuit)
							{
								while (auto iToDispatch = m_DispatchQueue.f_Pop())
									(*iToDispatch)();

								_pThread->m_EventWantQuit.f_Wait();
							}

							return 0;
						}
						, "Process launch proxy client"
					)
				;
			}

			NMib::NProcess::CProcessLaunchParams LaunchParams = _ServerLaunchParams;

			LaunchParams.m_bSeparateStdErr = true;
			LaunchParams.m_bThreaded = true;
			LaunchParams.m_Environment["MalterlibDisableStdErrLog"] = "true";
			LaunchParams.m_bMergeEnvironment = 1;

			LaunchParams.m_fOnStateChange
				= [this](CProcessLaunchStateChangeVariant const &_State, fp64 _TimeSinceStart)
				{
					switch (_State.f_GetTypeID())
					{
					case EProcessLaunchState_Launched:
						{
							if (!m_BufferedSendData.f_IsEmpty())
							{
								try
								{
									m_pServer->f_SendStdIn(fg_Move(m_BufferedSendData));
								}
								catch (NException::CException const &_Exception)
								{
									NStr::CStr Error = _Exception.f_GetErrorStr();
									for (auto iErrorHandler = m_BufferedSendErrors.f_GetIterator(); iErrorHandler; ++iErrorHandler)
									{
										(*iErrorHandler)(Error);
									}
								}

								m_BufferedSendErrors.f_Clear();
							}

							m_bLaunched = true;
						}
						break;
					case EProcessLaunchState_LaunchFailed:
						{
							f_SetError(_State.f_Get<EProcessLaunchState_LaunchFailed>());
							m_ServerExited.f_SetSignaled();
						}
						break;
					case EProcessLaunchState_Exited:
						{
							uint32 ExitCode = _State.f_Get<EProcessLaunchState_Exited>();
							if (ExitCode != 0 || !m_StdErr.f_IsEmpty())
								f_SetError(NStr::CStr::CFormat("The proxy server has exited with exit code {}: {}") << ExitCode << (m_StdErr + m_ErrorData + m_BufferedData));

							m_ServerExited.f_SetSignaled();
						}
						break;
					}
				}
			;

			LaunchParams.m_fOnOutput
				= [this](EProcessLaunchOutputType _OutputType, NMib::NStr::CStr const &_Output)
				{
					if (_OutputType == EProcessLaunchOutputType_StdOut)
						f_ReceiveData(_Output);
					else
						m_StdErr += _Output;
				}
			;

			LaunchParams.m_fDispatcher
				= [this](NFunction::TCFunction<void ()> const &_Functor)
				{
					f_Dispatch(_Functor);
				}
			;

			m_pServer = fg_Construct(LaunchParams, NMib::NProcess::EProcessLaunchCloseFlag_TerminateProcess | NMib::NProcess::EProcessLaunchCloseFlag_BlockOnExit);

			NPrivate::CConnect Connect;

			f_SendMessageAsync
				(
					Connect
					, [this](NConcurrency::TCAsyncResult<NPrivate::CConnect::CResponse> const &_Response)
					{
						try
						{
							_Response.f_Get();
						}
						catch (NException::CException const &_Exception)
						{
							f_SetError(_Exception.f_GetErrorStr());
						}
					}
				)
			;

		}

		~CInternal()
		{
			NPrivate::CDisconnect Disconnect;

			f_SendMessageAsync
				(
					Disconnect
					, [this](NConcurrency::TCAsyncResult<NPrivate::CDisconnect::CResponse> const &_Response)
					{
						try
						{
							_Response.f_Get();
						}
						catch (NException::CException const &_Exception)
						{
							f_SetError(_Exception.f_GetErrorStr());
						}
					}
				)
			;

			// Timeout after 5 seconds
			if (m_pServer)
			{
				if (m_ServerExited.f_WaitTimeout(5.0))
				{
					m_pServer->f_Close(NMib::NProcess::EProcessLaunchCloseFlag_TerminateProcess | NMib::NProcess::EProcessLaunchCloseFlag_BlockOnExit);
				}

				m_pServer.f_Clear();
			}
			{
				DMibLock(m_ThreadLock);
				m_pThreadObject.f_Clear();
			}
		}

	};
	CProxiedLaunchClient::CProxiedLaunchClient(NMib::NProcess::CProcessLaunchParams const &_ServerLaunchParams, NFunction::TCFunction<void (NStr::CStr const &_Error)> const &_OnError)
	{
		m_pInternal = fg_Construct(_ServerLaunchParams, _OnError);
	}

	CProxiedLaunchClient::~CProxiedLaunchClient()
	{
		m_pInternal.f_Clear();
	}

	FVirtualProcessLaunchFactory CProxiedLaunchClient::f_GetFactory()
	{
		return
			[this](CProcessLaunchParams const &_Params, EProcessLaunchCloseFlag _DestructFlags) -> NStorage::TCUniquePointer<CVirtualProcessLaunch>
			{
				CProcessLaunchParams Params = _Params;
				Params.m_bThreaded = true;
				return fg_Construct<CProxiedLaunchClient::CInternal::CVirtualProcessLaunch_Client>(m_pInternal.f_Get(), Params, _DestructFlags);
			}
		;
	}


	struct CProxiedLaunchServer::CInternal
	{
		NThread::CEvent m_DisconnectEvent;
		NStorage::TCUniquePointer<NThread::CThreadObject> m_pThreadObject;
		NThread::CMutual m_ThreadLock;

		NAtomic::TCAtomic<smint> m_MessageID;

		CInternal()
		{
		}
		~CInternal()
		{
			{
				DMibLock(m_ThreadLock);
				m_pThreadObject.f_Clear();
			}

			f_ClearLaunches();
		}

		void f_ClearLaunches()
		{
			DMibLock(m_LaunchStatesLock);
			for (auto iState = m_LaunchStates.f_GetIterator(); iState; ++iState)
			{
				(*iState)->f_SetServer(nullptr);
				if ((*iState)->m_pLaunch)
				{
					(*iState)->m_pLaunch->f_Close(EProcessLaunchCloseFlag_BlockOnExit | EProcessLaunchCloseFlag_TerminateProcess);
					(*iState)->m_pLaunch.f_Clear();
				}
			}
		}

		void f_SendPacket(NContainer::CByteVector const &_Data)
		{
			NStr::CStr Base64Data = NEncoding::fg_Base64Encode(_Data);
			Base64Data += "\n";
			NSys::fg_ConsoleOutputRaw(Base64Data);
		}

		void fp_Send(NContainer::CByteVector &&_Packet, NFunction::TCFunction<void (NStr::CStr const &_Error)> &&_fOnError)
		{
			f_SendPacket(_Packet);
		}

		template <typename tf_CMessage>
		void f_SendMessageAsync
			(
				tf_CMessage const &_Message
				, NFunction::TCFunction<void (NConcurrency::TCAsyncResult<typename tf_CMessage::CResponse> const &_Result)> const &_Response
			)
		;

		struct CLaunchState
		{
			NStorage::CIntrusiveRefCount m_RefCount;
			mutable NMib::NThread::CMutualManyRead mp_ServerLock;
			CProxiedLaunchServer::CInternal *mp_pServer;
			void f_SetServer(CProxiedLaunchServer::CInternal *_pServer)
			{
				DMibLock(mp_ServerLock);
				mp_pServer = _pServer;
			}
			class CServerReference
			{
				NMib::NThread::TCMovableScopeLockRead<NMib::NThread::CMutualManyRead> m_Lock;
				CProxiedLaunchServer::CInternal *mp_pServer;
			public:
				CServerReference(NMib::NThread::CMutualManyRead &_Lock, CProxiedLaunchServer::CInternal *_pServer)
					: mp_pServer(_pServer)
					, m_Lock(_Lock)
				{
				}
				CServerReference(CServerReference &&_Ref)
					: mp_pServer(_Ref.mp_pServer)
					, m_Lock(fg_Move(_Ref.m_Lock))
				{
					_Ref.mp_pServer = nullptr;
				}

				CProxiedLaunchServer::CInternal *f_GetServer() const
				{
					return mp_pServer;
				}

			};
			CServerReference f_GetServer() const
			{
				return CServerReference(mp_ServerLock, mp_pServer);
			}
			uint64 m_LaunchID;
			EProcessLaunchCloseFlag m_DestructFlags;

			NStorage::TCUniquePointer<CProcessLaunch> m_pLaunch;

			CLaunchState(CProxiedLaunchServer::CInternal *_pServer, uint64 _LaunchID, EProcessLaunchCloseFlag _DestructFlags);

			void f_ReportLaunchFailed(NStr::CStr const &_Error) const;
			void f_SendStdOutput(EProcessLaunchOutputType _OutputType, NMib::NStr::CStr const &_Output);
		};

		NThread::CMutual m_LaunchStatesLock;
		NContainer::TCMap<uint64, NStorage::TCSharedPointer<CLaunchState>> m_LaunchStates;

		void f_AddLaunchState(uint64 _LaunchID, NStorage::TCSharedPointer<CLaunchState> const &_State)
		{
			DMibLock(m_LaunchStatesLock);
			m_LaunchStates[_LaunchID] = _State;
		}
		void f_RemoveLaunchState(uint64 _LaunchID)
		{
			DMibLock(m_LaunchStatesLock);
			auto *pState = m_LaunchStates.f_FindEqual(_LaunchID);
			if (pState)
			{
				(*pState)->f_SetServer(nullptr);
				if ((*pState)->m_pLaunch)
				{
					(*pState)->m_pLaunch->f_Close(EProcessLaunchCloseFlag_BlockOnExit | EProcessLaunchCloseFlag_TerminateProcess);
					(*pState)->m_pLaunch = nullptr;
				}
				m_LaunchStates.f_Remove(_LaunchID);
			}
		}
		NStorage::TCSharedPointer<CLaunchState> f_GetLaunchState(uint64 _LaunchID)
		{
			DMibLock(m_LaunchStatesLock);
			auto *pState = m_LaunchStates.f_FindEqual(_LaunchID);
			if (pState)
				return *pState;
			return nullptr;
		}


		NThread::CMutual m_MessageHandlerLock;
		NContainer::TCMap<uint32, NFunction::TCFunction<void (NStream::CBinaryStreamMemoryPtr<> &_Stream)>> m_MessageHandlers;


		NStr::CStr m_BufferedData;

		void f_ReceivePacket(NStr::CStr const &_Packet);

		void f_ReceiveData(NStr::CStr const &_Data)
		{
			auto pParse = _Data.f_GetStr();

			while (*pParse)
			{
				auto pStart = pParse;
				NStr::fg_ParseToEndOfLine(pParse);
				if (pParse != pStart)
					m_BufferedData += NStr::CStr(pStart, pParse - pStart);
				if (*pParse == '\n' || *pParse == '\r')
				{
					f_ReceivePacket(m_BufferedData);
					m_BufferedData.f_Clear();
					NStr::fg_ParseEndOfLine(pParse);
				}
			}
		}

		NContainer::TCThreadSafeQueue<NFunction::TCFunctionMovable<void ()>> m_DispatchQueue;

		void f_Dispatch(NFunction::TCFunctionMovable<void ()> &&_ToDispatch)
		{
			m_DispatchQueue.f_Push(fg_Move(_ToDispatch));
			{
				DMibLock(m_ThreadLock);
				m_pThreadObject->m_EventWantQuit.f_Signal();
			}
		}

		void f_Init()
		{
			DMibLock(m_ThreadLock);
			m_pThreadObject = NThread::CThreadObject::fs_StartThread
				(
					[this](NThread::CThreadObject *_pThread) -> aint
					{
						CStdInReaderParams Params
							= CStdInReaderParams::fs_Create
							(
								[&](EStdInReaderOutputType _Type, NMib::NStr::CStr const &_Input)
								{
									if (_Type == EStdInReaderOutputType_StdIn)
										f_ReceiveData(_Input);
									else if (_Type == EStdInReaderOutputType_GeneralError)
										m_DisconnectEvent.f_SetSignaled();

								}
								, EStdInReaderFlag_Exclusive
								, [&](NFunction::TCFunctionMovable<void ()> &&_ToDispatch)
								{
									f_Dispatch(fg_Move(_ToDispatch));
								}
							)
						;

						CStdInReader Reader(fg_Move(Params));

						while (_pThread->f_GetState() != NThread::EThreadState_EventWantQuit)
						{
							while (auto iToDispatch = m_DispatchQueue.f_Pop())
								(*iToDispatch)();
							_pThread->m_EventWantQuit.f_Wait();
						}

						f_ClearLaunches();

						return 0;
					}
					, "Process launch proxy server"
				)
			;

		}
	};

	CProxiedLaunchServer::CProxiedLaunchServer()
	{
		m_pInternal = fg_Construct();
		m_pInternal->f_Init();
	}

	CProxiedLaunchServer::~CProxiedLaunchServer()
	{
		m_pInternal->m_DisconnectEvent.f_Wait();

		m_pInternal.f_Clear();
	}

	namespace NPrivate
	{
		void CConnect::f_HandleMessage(CProxiedLaunchClient::CInternal *_pClient, NFunction::TCFunction<void (CResponse &_Response)> const &_OnResponse)
		{
			DMibErrorProcessProxyProtocol("Not valid on client");
		}
		void CConnect::f_HandleMessage(CProxiedLaunchServer::CInternal *_pServer, NFunction::TCFunction<void (CResponse &_Response)> const &_OnResponse)
		{
			CResponse Response;
			_OnResponse(Response);
		}

		void CDisconnect::f_HandleMessage(CProxiedLaunchClient::CInternal *_pClient, NFunction::TCFunction<void (CResponse &_Response)> const &_OnResponse)
		{
			DMibErrorProcessProxyProtocol("Not valid on client");
		}
		void CDisconnect::f_HandleMessage(CProxiedLaunchServer::CInternal *_pServer, NFunction::TCFunction<void (CResponse &_Response)> const &_OnResponse)
		{
			CResponse Response;
			_OnResponse(Response);
			_pServer->m_DisconnectEvent.f_SetSignaled();
		}

		void CProcessLaunch_Launch::f_HandleMessage(CProxiedLaunchClient::CInternal *_pClient, NFunction::TCFunction<void (CResponse &_Response)> const &_OnResponse)
		{
			DMibErrorProcessProxyProtocol("Not valid on client");
		}
		void CProcessLaunch_Launch::f_HandleMessage(CProxiedLaunchServer::CInternal *_pServer, NFunction::TCFunction<void (CResponse &_Response)> const &_OnResponse)
		{
			NStorage::TCSharedPointer<CProxiedLaunchServer::CInternal::CLaunchState> pState
				= fg_Construct
				(
					_pServer
					, m_LaunchID
					, m_DestructFlags
				)
			;

			NStorage::TCSharedPointer<COnScopeExitShared> pCleanupLaunch = fg_Construct();

			auto Params = m_Params;
			Params.m_fOnStateChange
				= [pState, pCleanupLaunch](CProcessLaunchStateChangeVariant const &_State, fp64 _TimeSinceStart)
				{
					CProcessLaunch_StateChange Message(pState->m_LaunchID, _State, _TimeSinceStart);

					auto pState2 = pState;
					auto Server = pState->f_GetServer();
					if (Server.f_GetServer())
					{
						Server.f_GetServer()->f_SendMessageAsync
							(
								Message
								, [pState2](NConcurrency::TCAsyncResult<CProcessLaunch_StateChange::CResponse> const &_Result)
								{
									if (!_Result)
									{
										auto Server = pState2->f_GetServer();
										pState2->m_pLaunch.f_Clear();
										if (Server.f_GetServer())
											Server.f_GetServer()->f_RemoveLaunchState(pState2->m_LaunchID);
									}
								}
							)
						;

						switch (_State.f_GetTypeID())
						{
						case EProcessLaunchState_Launched:
							break;
						case EProcessLaunchState_LaunchFailed:
						case EProcessLaunchState_Exited:
							// Clear out any references to lambdas
							pState->m_pLaunch.f_Clear();
							break;
						}
					}
				}
			;

			Params.m_fOnOutput
				= [pState](EProcessLaunchOutputType _OutputType, NMib::NStr::CStr const &_Output)
				{
					pState->f_SendStdOutput(_OutputType, _Output);
				}
			;

			Params.m_fDispatcher
				= [pState](NFunction::TCFunction<void ()> const &_ToDispatch)
				{
					auto Server = pState->f_GetServer();
					if (Server.f_GetServer())
						Server.f_GetServer()->f_Dispatch(_ToDispatch);
				}
			;

			_pServer->f_AddLaunchState(pState->m_LaunchID, pState);

			_pServer->f_Dispatch
				(
					[pState, _OnResponse, Params, pCleanupLaunch]()
					{
						pState->m_pLaunch = fg_Construct(Params, pState->m_DestructFlags);
						CResponse Reponse;
						_OnResponse(Reponse);
					}
				)
			;
		}

		void CProcessLaunch_StateChange::f_HandleMessage(CProxiedLaunchClient::CInternal *_pClient, NFunction::TCFunction<void (CResponse &_Response)> const &_OnResponse)
		{
			auto pState = _pClient->f_GetLaunchState(m_LaunchID);

			if (pState)
			{
				CProcessLaunchStateChangeVariant Variant;
				bool bFinished = false;
				bool bReportChange = true;
				switch (m_State.f_GetTypeID())
				{
				case EProcessLaunchState_Launched:
					{
						Variant = (void *)nullptr;
					}
					break;
				case EProcessLaunchState_LaunchFailed:
					{
						bReportChange = pState->m_bIsRunning.f_Exchange(false);
						bFinished = true;
						Variant = m_State.f_Get<EProcessLaunchState_LaunchFailed>();
					}
					break;
				case EProcessLaunchState_Exited:
					{
						bReportChange = pState->m_bIsRunning.f_Exchange(false);
						bFinished = true;
						Variant = m_State.f_Get<EProcessLaunchState_Exited>();
					}
					break;
				}

				if (bReportChange)
				{
					pState->f_ReportStateChange(pState, Variant, m_TimeSinceStart);
					if (bFinished)
						pState->m_FinishedEvent.f_SetSignaled();
				}
			}
			CResponse Reponse;
			_OnResponse(Reponse);
		}
		void CProcessLaunch_StateChange::f_HandleMessage(CProxiedLaunchServer::CInternal *_pServer, NFunction::TCFunction<void (CResponse &_Response)> const &_OnResponse)
		{
			DMibErrorProcessProxyProtocol("Not valid on server");
		}

		void CProcessLaunch_Output::f_HandleMessage(CProxiedLaunchClient::CInternal *_pClient, NFunction::TCFunction<void (CResponse &_Response)> const &_OnResponse)
		{
			auto pState = _pClient->f_GetLaunchState(m_LaunchID);

			if (pState)
				pState->f_ReportOutput(pState, m_OutputType, m_Output);

			CResponse Reponse;
			_OnResponse(Reponse);
		}
		void CProcessLaunch_Output::f_HandleMessage(CProxiedLaunchServer::CInternal *_pServer, NFunction::TCFunction<void (CResponse &_Response)> const &_OnResponse)
		{
			DMibErrorProcessProxyProtocol("Not valid on server");
		}

		void CProcessLaunch_Input::f_HandleMessage(CProxiedLaunchClient::CInternal *_pClient, NFunction::TCFunction<void (CResponse &_Response)> const &_OnResponse)
		{
			DMibErrorProcessProxyProtocol("Not valid on client");
		}
		void CProcessLaunch_Input::f_HandleMessage(CProxiedLaunchServer::CInternal *_pServer, NFunction::TCFunction<void (CResponse &_Response)> const &_OnResponse)
		{
			auto pState = _pServer->f_GetLaunchState(m_LaunchID);
			if (pState && pState->m_pLaunch && pState->m_pLaunch->f_IsOpen())
				pState->m_pLaunch->f_SendStdIn(this->m_Input);
			CResponse Reponse;
			_OnResponse(Reponse);
		}

		void CProcessLaunch_CloseInput::f_HandleMessage(CProxiedLaunchClient::CInternal *_pClient, NFunction::TCFunction<void (CResponse &_Response)> const &_OnResponse)
		{
			DMibErrorProcessProxyProtocol("Not valid on client");
		}
		void CProcessLaunch_CloseInput::f_HandleMessage(CProxiedLaunchServer::CInternal *_pServer, NFunction::TCFunction<void (CResponse &_Response)> const &_OnResponse)
		{
			auto pState = _pServer->f_GetLaunchState(m_LaunchID);
			if (pState && pState->m_pLaunch && pState->m_pLaunch->f_IsOpen())
				pState->m_pLaunch->f_CloseStdIn();
			CResponse Reponse;
			_OnResponse(Reponse);
		}

		void CProcessLaunch_Close::f_HandleMessage(CProxiedLaunchClient::CInternal *_pClient, NFunction::TCFunction<void (CResponse &_Response)> const &_OnResponse)
		{
			DMibErrorProcessProxyProtocol("Not valid on client");
		}
		void CProcessLaunch_Close::f_HandleMessage(CProxiedLaunchServer::CInternal *_pServer, NFunction::TCFunction<void (CResponse &_Response)> const &_OnResponse)
		{
			auto pState = _pServer->f_GetLaunchState(m_LaunchID);
			if (pState && pState->m_pLaunch && pState->m_pLaunch->f_IsOpen())
				pState->m_pLaunch->f_Close(this->m_CloseFlag);
			CResponse Reponse;
			_OnResponse(Reponse);
		}

		void CProcessLaunch_Stop::f_HandleMessage(CProxiedLaunchClient::CInternal *_pClient, NFunction::TCFunction<void (CResponse &_Response)> const &_OnResponse)
		{
			DMibErrorProcessProxyProtocol("Not valid on client");
		}
		void CProcessLaunch_Stop::f_HandleMessage(CProxiedLaunchServer::CInternal *_pServer, NFunction::TCFunction<void (CResponse &_Response)> const &_OnResponse)
		{
			auto pState = _pServer->f_GetLaunchState(m_LaunchID);
			if (pState && pState->m_pLaunch && pState->m_pLaunch->f_IsOpen())
				pState->m_pLaunch->f_StopProcess();
			CResponse Reponse;
			_OnResponse(Reponse);
		}

		struct CHandleMessageVisitor
		{
			CProxiedLaunchClient::CInternal *m_pClient;
			CProxiedLaunchServer::CInternal *m_pServer;

			NStream::CBinaryStreamMemoryPtr<> &m_Stream;
			NFunction::TCFunction<void (NContainer::CByteVector &&_SendReply)> m_fOnReply;

			uint32 m_MessageID;

			CHandleMessageVisitor
				(
					CProxiedLaunchClient::CInternal *_pClient
					, NStream::CBinaryStreamMemoryPtr<> &_Stream
					, uint32 _MessageID
					, NFunction::TCFunction<void (NContainer::CByteVector &&_SendReply)> &&_fOnReply
				)
				: m_pClient(_pClient)
				, m_pServer(nullptr)
				, m_Stream(_Stream)
				, m_MessageID(_MessageID)
				, m_fOnReply(fg_Move(_fOnReply))
			{
			}

			CHandleMessageVisitor
				(
					CProxiedLaunchServer::CInternal *_pServer
					, NStream::CBinaryStreamMemoryPtr<> &_Stream
					, uint32 _MessageID
					, NFunction::TCFunction<void (NContainer::CByteVector &&_SendReply)> &&_fOnReply
				)
				: m_pClient(nullptr)
				, m_pServer(_pServer)
				, m_Stream(_Stream)
				, m_MessageID(_MessageID)
				, m_fOnReply(fg_Move(_fOnReply))
			{
			}

			template <typename t_CType>
			void operator ()()
			{
				t_CType Message;

				try
				{
					m_Stream >> Message;
				}
				catch (NException::CException const &_Error)
				{
					DMibErrorProcessProxyProtocol(_Error.f_GetErrorStr());
				}
				auto fOnReply = fg_Move(m_fOnReply);
				auto MessageID = m_MessageID;
				if (m_pServer)
				{
					Message.f_HandleMessage
						(
							m_pServer
							, [fOnReply, MessageID](typename t_CType::CResponse const &_Response)
							{
								NStream::CBinaryStreamMemory<> Stream;
								Stream << uint8(1); // Reply
								Stream << MessageID;
								Stream << _Response;

								fOnReply(Stream.f_MoveVectorOptimized());
							}
						)
					;
				}
				else
				{
					Message.f_HandleMessage
						(
							m_pClient
							, [fOnReply, MessageID](typename t_CType::CResponse const &_Response)
							{
								NStream::CBinaryStreamMemory<> Stream;
								Stream << uint8(1); // Reply
								Stream << MessageID;
								Stream << _Response;

								fOnReply(Stream.f_MoveVectorOptimized());
							}
						)
					;
				}
			}
		};

	}

	CProxiedLaunchClient::CInternal::CVirtualProcessLaunch_Client::CVirtualProcessLaunch_Client(CProxiedLaunchClient::CInternal *_pClient, CProcessLaunchParams const &_Params, EProcessLaunchCloseFlag _DestructFlags)
		: m_pState(fg_Construct(_pClient, _Params, _DestructFlags))
	{
		_pClient->f_AddLaunchState(m_pState->m_LaunchID, m_pState);

		EProcessLaunchCloseFlag RemoteCloseFlags = _DestructFlags & EProcessLaunchCloseFlag_TerminateProcess;

		NStorage::TCSharedPointer<CLaunchState> pState = m_pState;
		NPrivate::CProcessLaunch_Launch Launch(_Params, RemoteCloseFlags, m_pState->m_LaunchID);
		m_pState->m_RunningTime.f_Start();
		m_pState->m_bOpen.f_Exchange(true);
		m_pState->m_bIsRunning.f_Exchange(true);
		_pClient->f_SendMessageAsync
			(
				Launch
				, [pState](NConcurrency::TCAsyncResult<NPrivate::CProcessLaunch_Launch::CResponse> const &_Response)
				{
					try
					{
						_Response.f_Get();
					}
					catch (NException::CException const &_Exception)
					{
						if (pState->m_bIsRunning.f_Exchange(false))
						{
							pState->f_ReportLaunchFailed(pState, _Exception.f_GetErrorStr());
							pState->m_FinishedEvent.f_SetSignaled();
						}
					}
				}
			)
		;
	}

	CProxiedLaunchClient::CInternal::CVirtualProcessLaunch_Client::~CVirtualProcessLaunch_Client()
	{
		f_Close(m_pState->m_DestructFlags);
	}

	EProcessLaunchCloseFlag CProxiedLaunchClient::CInternal::CVirtualProcessLaunch_Client::f_GetCloseFlags() const
	{
		return m_pState->m_DestructFlags;
	}

	void CProxiedLaunchClient::CInternal::CVirtualProcessLaunch_Client::f_StopProcess() const
	{
		if (!m_pState->m_bOpen.f_Load())
			return;

		NStorage::TCSharedPointer<CLaunchState> pState = m_pState;

		NPrivate::CProcessLaunch_Stop Message(pState->m_LaunchID);
		pState->m_pClient->f_SendMessageAsync
			(
				Message
				, [pState](NConcurrency::TCAsyncResult<NPrivate::CProcessLaunch_Stop::CResponse> const &_Response)
				{
					try
					{
						_Response.f_Get();
					}
					catch (NException::CException const &_Exception)
					{
						pState->f_ReportOutput(pState, EProcessLaunchOutputType_GeneralError, NStr::CStr::CFormat("error: {}" DMibNewLine) << _Exception.f_GetErrorStr());
					}
				}
			)
		;
	}

	void CProxiedLaunchClient::CInternal::CVirtualProcessLaunch_Client::f_Close(EProcessLaunchCloseFlag _CloseFlags)
	{
		if (m_pState->m_bOpen.f_Exchange(false))
		{
			NStorage::TCSharedPointer<CLaunchState> pState = m_pState;

			if (_CloseFlags & (EProcessLaunchCloseFlag_TerminateProcess | EProcessLaunchCloseFlag_StopProcess))
			{
				// We only need to send a message to remote if we need to terminate the process
				EProcessLaunchCloseFlag RemoteCloseFlags = (_CloseFlags & (EProcessLaunchCloseFlag_TerminateProcess | EProcessLaunchCloseFlag_StopProcess)) | EProcessLaunchCloseFlag_LingerUntilDone;

				// Always linger on the remote side

				NPrivate::CProcessLaunch_Close Message(pState->m_LaunchID, RemoteCloseFlags);
				pState->m_pClient->f_SendMessageAsync
					(
						Message
						, [pState](NConcurrency::TCAsyncResult<NPrivate::CProcessLaunch_Close::CResponse> const &_Response)
						{
							try
							{
								_Response.f_Get();
							}
							catch (NException::CException const &_Exception)
							{
								pState->f_ReportOutput(pState, EProcessLaunchOutputType_GeneralError, NStr::CStr::CFormat("error: {}" DMibNewLine) << _Exception.f_GetErrorStr());
							}
						}
					)
				;
			}

			m_pState->m_DestructFlags = _CloseFlags;

			if (_CloseFlags & EProcessLaunchCloseFlag_BlockOnExit)
				m_pState->m_FinishedEvent.f_Wait();

			if (!(_CloseFlags & EProcessLaunchCloseFlag_LingerUntilDone))
				m_pState->m_pClient->f_RemoveLaunchState(m_pState->m_LaunchID);
		}
	}

	bool CProxiedLaunchClient::CInternal::CVirtualProcessLaunch_Client::f_IsOpen() const
	{
		return m_pState->m_bOpen.f_Load();
	}

	bool CProxiedLaunchClient::CInternal::CVirtualProcessLaunch_Client::f_IsRunning() const
	{
		return m_pState->m_bIsRunning.f_Load();
	}

	void CProxiedLaunchClient::CInternal::CVirtualProcessLaunch_Client::f_SendStdIn(NMib::NStr::CStrIO const &_Data) const
	{
		NStorage::TCSharedPointer<CLaunchState> pState = m_pState;
		NPrivate::CProcessLaunch_Input Message(pState->m_LaunchID, _Data);
		pState->m_pClient->f_SendMessageAsync
			(
				Message
				, [pState](NConcurrency::TCAsyncResult<NPrivate::CProcessLaunch_Input::CResponse> const &_Response)
				{
					try
					{
						_Response.f_Get();
					}
					catch (NException::CException const &_Exception)
					{
						pState->f_ReportOutput(pState, EProcessLaunchOutputType_GeneralError, NStr::CStr::CFormat("error: {}" DMibNewLine) << _Exception.f_GetErrorStr());
					}
				}
			)
		;
	}

	void CProxiedLaunchClient::CInternal::CVirtualProcessLaunch_Client::f_CloseStdIn() const
	{
		NStorage::TCSharedPointer<CLaunchState> pState = m_pState;
		NPrivate::CProcessLaunch_CloseInput Message(pState->m_LaunchID);
		pState->m_pClient->f_SendMessageAsync
			(
				Message
				, [pState](NConcurrency::TCAsyncResult<NPrivate::CProcessLaunch_CloseInput::CResponse> const &_Response)
				{
					try
					{
						_Response.f_Get();
					}
					catch (NException::CException const &_Exception)
					{
						pState->f_ReportOutput(pState, EProcessLaunchOutputType_GeneralError, NStr::CStr::CFormat("error: {}" DMibNewLine) << _Exception.f_GetErrorStr());
					}
				}
			)
		;
	}

	fp64 CProxiedLaunchClient::CInternal::CVirtualProcessLaunch_Client::f_GetRunningTime() const
	{
		return m_pState->f_GetRunningTime();
	}


	void CProxiedLaunchClient::CInternal::f_ReceivePacket(NStr::CStr const &_Packet)
	{
		NContainer::CByteVector Output;
		try
		{
			NEncoding::fg_Base64Decode(_Packet, Output);
		}
		catch (NException::CException const &)
		{
			m_ErrorData += _Packet;
			m_ErrorData += "\n";
			f_SetError("Unrecognized std output from proxy server: " + m_ErrorData);
			return;
		}
		NStream::CBinaryStreamMemoryPtr<> Stream;
		Stream.f_OpenRead(Output);
		uint8 bIsReply;
		uint32 MessageID;
		try
		{
			Stream >> bIsReply;
			Stream >> MessageID;
		}
		catch (NException::CException const &_Exception)
		{
			m_ErrorData += _Packet;
			m_ErrorData += "\n";
			f_SetError(NStr::CStr::CFormat("Error reading packet ({}): {}") << _Exception.f_GetErrorStr() << m_ErrorData);
			return;
		}

		try
		{
			if (bIsReply)
			{
				NFunction::TCFunction<void (NStream::CBinaryStreamMemoryPtr<> &_Stream)> Handler;
				{
					DMibLock(m_MessageHandlerLock);
					auto *pHandler = m_MessageHandlers.f_FindEqual(MessageID);
					if (!pHandler)
						return;
					Handler = fg_Move(*pHandler);
					m_MessageHandlers.f_Remove(MessageID);
				}
				Handler(Stream);
			}
			else
			{
				uint32 MessageType;
				Stream >> MessageType;

				NPrivate::CHandleMessageVisitor Visitor
					(
						this
						, Stream
						, MessageID
						, [this](NContainer::CByteVector &&_SendReply)
						{
							f_SendPacket(_SendReply, NFunction::TCFunction<void (NStr::CStr const &_Error)>());
						}
					)
				;

				if (!NPrivate::fg_VisitType(Visitor, MessageType))
				{
					m_ErrorData += _Packet;
					m_ErrorData += "\n";
					f_SetError(NStr::CStr::CFormat("Error reading packet (Unrecognized packet type): {}") << m_ErrorData);
					return;
				}
			}
		}
		catch (CExceptionProcessProxyProtocol const &_Exception)
		{
			m_ErrorData += _Packet;
			m_ErrorData += "\n";
			f_SetError(NStr::CStr::CFormat("Error reading packet ({}): {}") << _Exception.f_GetErrorStr() << m_ErrorData);
			return;

		}

	}

	void CProxiedLaunchServer::CInternal::f_ReceivePacket(NStr::CStr const &_Packet)
	{
		NContainer::CByteVector Output;
		NEncoding::fg_Base64Decode(_Packet, Output);
		NStream::CBinaryStreamMemoryPtr<> Stream;
		Stream.f_OpenRead(Output);

		uint8 bIsReply;
		Stream >> bIsReply;

		uint32 MessageID;
		Stream >> MessageID;

		if (bIsReply)
		{
			NFunction::TCFunction<void (NStream::CBinaryStreamMemoryPtr<> &_Stream)> Handler;
			{
				DMibLock(m_MessageHandlerLock);
				auto *pHandler = m_MessageHandlers.f_FindEqual(MessageID);
				if (!pHandler)
					return;
				Handler = fg_Move(*pHandler);
				m_MessageHandlers.f_Remove(MessageID);
			}
			Handler(Stream);
		}
		else
		{
			uint32 MessageType;
			Stream >> MessageType;

			NPrivate::CHandleMessageVisitor Visitor
				(
					this
					, Stream
					, MessageID
					, [this](NContainer::CByteVector &&_SendReply)
					{
						f_SendPacket(_SendReply);
					}
				)
			;

			NPrivate::fg_VisitType
				(
					Visitor
					, MessageType
				)
			;
		}
	}

	template <typename tf_CMessage>
	void CProxiedLaunchClient::CInternal::f_SendMessageAsync
		(
			tf_CMessage const &_Message
			, NFunction::TCFunction<void (NConcurrency::TCAsyncResult<typename tf_CMessage::CResponse> const &_Result)> const &_Response
		)
	{
		NStorage::TCSharedPointer<NAtomic::TCAtomic<smint>> pErrorHandled = fg_Construct();
		using CResponse = typename tf_CMessage::CResponse;
		auto pErrorScope
			= fg_OnScopeExit
			(
				[_Response, pErrorHandled]
				{
					if (!pErrorHandled->f_Exchange(1))
					{
						NConcurrency::TCAsyncResult<CResponse> Response;
						Response.f_SetException(DMibImpExceptionInstance(CExceptionProcessProxyProtocol, "Failed to send message to proxy launch server"));
						_Response(Response);
					}
				}
			)
		;
		auto pErrorLambda = fg_LambdaMove(pErrorScope);


		uint32 MessageID = ++m_MessageID;
		NStream::CBinaryStreamMemory<> Stream;
		Stream << uint8(0); // Not reply
		Stream << MessageID;
		uint32 TypeID = NPrivate::TCTypeToID<tf_CMessage>::mc_Value;
		Stream << TypeID;
		Stream << _Message;


		{
			DMibLock(m_MessageHandlerLock);
			m_MessageHandlers[MessageID]
				= [_Response, pErrorLambda, pErrorHandled](NStream::CBinaryStreamMemoryPtr<> &_Stream)
				{
					if (!pErrorHandled->f_Exchange(1))
					{
						NConcurrency::TCAsyncResult<CResponse> Result;
						try
						{
							CResponse Response;
							_Stream >> Response;
							Result.f_SetResult(fg_Move(Response));
						}
						catch (...)
						{
							Result.f_SetCurrentException();
						}
						(*pErrorLambda).f_Clear();
						_Response(Result);
					}
				}
			;
		}

		NContainer::CByteVector ToSend = Stream.f_MoveVectorOptimized();
		fp_Send
			(
				fg_Move(ToSend)
				, [_Response, pErrorHandled](NStr::CStr const &_Error)
				{
					if (!pErrorHandled->f_Exchange(1))
					{
						NConcurrency::TCAsyncResult<CResponse> Response;
						Response.f_SetException(DMibImpExceptionInstance(CExceptionProcessProxyProtocol, NStr::CStr::CFormat("Error sending message to proxy launch server: {}") << _Error));
						_Response(Response);
					}
				}
			)
		;
	}

	template <typename tf_CMessage>
	void CProxiedLaunchServer::CInternal::f_SendMessageAsync
		(
			tf_CMessage const &_Message
			, NFunction::TCFunction<void (NConcurrency::TCAsyncResult<typename tf_CMessage::CResponse> const &_Result)> const &_Response
		)
	{
		NStorage::TCSharedPointer<NAtomic::TCAtomic<smint>> pErrorHandled = fg_Construct();
		using CResponse = typename tf_CMessage::CResponse;
		auto pErrorScope
			= fg_OnScopeExit
			(
				[_Response, pErrorHandled]
				{
					if (!pErrorHandled->f_Exchange(1))
					{
						NConcurrency::TCAsyncResult<CResponse> Response;
						Response.f_SetException(DMibImpExceptionInstance(CExceptionProcessProxyProtocol, "Failed to send message to proxy launch server"));
						_Response(Response);
					}
				}
			)
		;
		auto pErrorLambda = fg_LambdaMove(pErrorScope);


		uint32 MessageID = ++m_MessageID;
		NStream::CBinaryStreamMemory<> Stream;
		Stream << uint8(0); // Not reply
		Stream << MessageID;
		uint32 TypeID = NPrivate::TCTypeToID<tf_CMessage>::mc_Value;
		Stream << TypeID;
		Stream << _Message;


		{
			DMibLock(m_MessageHandlerLock);
			m_MessageHandlers[MessageID]
				= [_Response, pErrorLambda, pErrorHandled](NStream::CBinaryStreamMemoryPtr<> &_Stream)
				{
					if (!pErrorHandled->f_Exchange(1))
					{
						NConcurrency::TCAsyncResult<CResponse> Result;
						try
						{
							CResponse Response;
							_Stream >> Response;
							Result.f_SetResult(fg_Move(Response));
						}
						catch (...)
						{
							Result.f_SetCurrentException();
						}
						(*pErrorLambda).f_Clear();
						_Response(Result);
					}
				}
			;
		}

		NContainer::CByteVector ToSend = Stream.f_MoveVectorOptimized();
		fp_Send
			(
				fg_Move(ToSend)
				, [_Response, pErrorHandled](NStr::CStr const &_Error)
				{
					if (!pErrorHandled->f_Exchange(1))
					{
						NConcurrency::TCAsyncResult<CResponse> Response;
						Response.f_SetException
							(
								DMibImpExceptionInstance(CExceptionProcessProxyProtocol, NStr::CStr::CFormat("Error sending message to proxy launch server: {}") << _Error)
							)
						;
						_Response(Response);
					}
				}
			)
		;
	}

	CProxiedLaunchClient::CInternal::CLaunchState::CLaunchState(CProxiedLaunchClient::CInternal *_pClient, CProcessLaunchParams const &_Params, EProcessLaunchCloseFlag _DestructFlags)
		: m_pClient(_pClient)
		, m_Params(_Params)
		, m_DestructFlags(_DestructFlags)
		, m_LaunchID(_pClient->f_NewLaunchID())
		, m_bOpen(false)
		, m_bIsRunning(false)
	{
	}

	fp64 CProxiedLaunchClient::CInternal::CLaunchState::f_GetRunningTime() const
	{
		DMibLock(m_RunningTimeLock);
		return m_RunningTime.f_GetTime();
	}

	void CProxiedLaunchClient::CInternal::CLaunchState::f_ReportLaunchFailed(NStorage::TCSharedPointer<CLaunchState> const &_pState, NStr::CStr const &_ErrorStr) const
	{
		f_ReportStateChange(_pState, _ErrorStr, f_GetRunningTime());
	}

	void CProxiedLaunchClient::CInternal::CLaunchState::f_ReportStateChange
		(
			NStorage::TCSharedPointer<CLaunchState> const &_pState
			, CProcessLaunchStateChangeVariant const &_State
			, fp64 _Time
		) const
	{
		if (m_Params.m_fOnStateChange)
		{
			if (m_Params.m_fDispatcher)
			{
				auto State = _State;
				m_Params.m_fDispatcher
					(
						[_pState, State, _Time]
						{
							_pState->m_Params.m_fOnStateChange(State, _Time);
						}
					)
				;
			}
			else
			{
				m_Params.m_fOnStateChange(_State, _Time);
			}
		}
	}

	void CProxiedLaunchClient::CInternal::CLaunchState::f_ReportOutput
		(
			NStorage::TCSharedPointer<CLaunchState> const &_pState
			, EProcessLaunchOutputType _OutputType
			, NMib::NStr::CStr const &_Output
		) const
	{
		if (m_Params.m_fOnOutput)
		{
			if (m_Params.m_fDispatcher)
			{
				m_Params.m_fDispatcher
					(
						[_pState, _OutputType, _Output]
						{
							_pState->m_Params.m_fOnOutput(_OutputType, _Output);
						}
					)
				;
			}
			else
			{
				m_Params.m_fOnOutput(_OutputType, _Output);
			}
		}

	}

	CProxiedLaunchServer::CInternal::CLaunchState::CLaunchState(CProxiedLaunchServer::CInternal *_pServer, uint64 _LaunchID, EProcessLaunchCloseFlag _DestructFlags)
		: mp_pServer(_pServer)
		, m_LaunchID(_LaunchID)
		, m_DestructFlags(_DestructFlags)
	{
	}

	void CProxiedLaunchServer::CInternal::CLaunchState::f_ReportLaunchFailed(NStr::CStr const &_Error) const
	{
		NPrivate::CProcessLaunch_StateChange Message(m_LaunchID, _Error, 0.0);

		auto Server = f_GetServer();

		if (Server.f_GetServer())
		{
			Server.f_GetServer()->f_SendMessageAsync
				(
					Message
					, [](NConcurrency::TCAsyncResult<NPrivate::CProcessLaunch_StateChange::CResponse> const &_Result)
					{
					}
				)
			;
		}
	}

	void CProxiedLaunchServer::CInternal::CLaunchState::f_SendStdOutput(EProcessLaunchOutputType _OutputType, NMib::NStr::CStr const &_Output)
	{
		if (!_Output.f_IsEmpty())
		{
			NPrivate::CProcessLaunch_Output Message(m_LaunchID, _OutputType, _Output);

			auto Server = f_GetServer();

			if (Server.f_GetServer())
			{
				NStorage::TCSharedPointer<CLaunchState> pState2 = fg_Explicit(&fg_RemoveQualifiers(*this));
				Server.f_GetServer()->f_SendMessageAsync
					(
						Message
						, [pState2](NConcurrency::TCAsyncResult<NPrivate::CProcessLaunch_Output::CResponse> const &_Result)
						{
							if (!_Result)
							{
								pState2->m_pLaunch.f_Clear();
								auto Server = pState2->f_GetServer();
								if (Server.f_GetServer())
									Server.f_GetServer()->f_RemoveLaunchState(pState2->m_LaunchID);
							}
						}
					)
				;
			}
		}
	}
}
