// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <Mib/Core/Core>
#include <Mib/Process/VirtualProcessLaunch>

namespace NMib::NProcess
{
	DMibImpErrorClassDefine(CExceptionProcessProxyProtocol, NException::CException);

#	define DMibErrorProcessProxyProtocol(_Description) DMibImpError(NMib::NProcess::CExceptionProcessProxyProtocol, _Description)

	class CProxiedLaunchClient
	{
	public:
		struct CInternal;
	private:
		NStorage::TCUniquePointer<CInternal> m_pInternal;

	public:

		FVirtualProcessLaunchFactory f_GetFactory();

		CProxiedLaunchClient(NMib::NProcess::CProcessLaunchParams const &_ServerLaunchParams, NFunction::TCFunction<void (NStr::CStr const &_Error)> const &_OnError);
		~CProxiedLaunchClient();
	};

	class CProxiedLaunchServer
	{
	public:
		struct CInternal;
	private:
		NStorage::TCUniquePointer<CInternal> m_pInternal;
	public:

		CProxiedLaunchServer();
		~CProxiedLaunchServer();
	};
}

#ifndef DMibPNoShortCuts
	using namespace NMib::NProcess;
#endif

