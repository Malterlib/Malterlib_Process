// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

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
