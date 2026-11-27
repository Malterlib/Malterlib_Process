// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <Mib/Process/ProcessLaunch>

namespace NMib::NProcess::NPlatform
{
	NStr::CStr fg_ExpandEnvironmentVars(NStr::CStr const &_Path);
}
