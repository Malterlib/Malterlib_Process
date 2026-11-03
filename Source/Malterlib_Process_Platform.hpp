// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

namespace NMib::NProcess
{
	template <typename tf_CStr>
	void CProcessStat::f_Format(tf_CStr &o_Str) const
	{
		NStr::CStr Unit;
		auto Value = f_GetValueWithScaledUnit(Unit);
		o_Str += typename tf_CStr::CFormat("{} {}") << Value << Unit;
	}

	template <typename tf_CStr>
	void CProcessStatistics::f_Format(tf_CStr &o_Str) const
	{
		o_Str += typename tf_CStr::CFormat("{}") << m_Statistics;
	}
}
