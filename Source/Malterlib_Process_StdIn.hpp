// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

namespace NMib::NProcess
{
	template <typename tf_CStream>
	void CStdInReaderPromptParams::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_Prompt;
		_Stream % m_bPassword;
	}
}
