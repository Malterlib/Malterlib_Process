// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <Mib/Core/Core>
#include <Mib/Process/VirtualProcessLaunch>

namespace NMib::NProcess
{
	DMibImpErrorClassDefine(CExceptionProcessProxyProtocol, NException::CException);

#define DMibErrorProcessProxyProtocol(d_Description) DMibImpError(NMib::NProcess::CExceptionProcessProxyProtocol, d_Description)

	struct CProxiedLaunchClient
	{
		struct CInternal;

		CProxiedLaunchClient(NMib::NProcess::CProcessLaunchParams const &_ServerLaunchParams, NFunction::TCFunction<void (NStr::CStr const &_Error)> const &_fOnError);
		~CProxiedLaunchClient();

		FVirtualProcessLaunchFactory f_GetFactory();

	private:
		NStorage::TCUniquePointer<CInternal> mp_pInternal;
	};

	struct CProxiedLaunchServer
	{
		struct CInternal;

		CProxiedLaunchServer();
		~CProxiedLaunchServer();

	private:
		NStorage::TCUniquePointer<CInternal> mp_pInternal;
	};
}

#ifndef DMibPNoShortCuts
using namespace NMib::NProcess;
#endif

