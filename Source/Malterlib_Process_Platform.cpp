// Copyright © 2023 Favro Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>

#include "Malterlib_Process_Platform.h"

namespace NMib::NProcess
{
	CProcessStat::CProcessStat(EProcessStatUnit _Unit, fp64 _Value, fp64 _IdealScale, NMib::NStr::CStr const &_CustomUnit)
		: m_Unit(_Unit)
		, m_CustomUnit(_CustomUnit)
		, m_Value(_Value)
		, m_IdealScale(_IdealScale)
	{
	}

	fp64 CProcessStat::f_GetValueWithScaledUnit(NStr::CStr &o_Unit) const
	{
		fp64 Value = m_Value;
		switch (m_Unit)
		{
		case NMib::NProcess::EProcessStatUnit_GeneralNumber:
			o_Unit = m_CustomUnit;
			break;
		case NMib::NProcess::EProcessStatUnit_Bytes:
			{
				if (m_IdealScale == fp64(1024*1024*1024))
				{
					Value /= m_IdealScale;
					o_Unit = NStr::gc_Str<"GiB">;
				}
				else if (m_IdealScale == fp64(1024*1024))
				{
					Value /= m_IdealScale;
					o_Unit = NStr::gc_Str<"MiB">;
				}
				else if (m_IdealScale == fp64(1024))
				{
					Value /= m_IdealScale;
					o_Unit = NStr::gc_Str<"KiB">;
				}
				else
					o_Unit = NStr::gc_Str<"Bytes">;
			}
			break;
		case NMib::NProcess::EProcessStatUnit_Cycles:
			{
				o_Unit = NStr::gc_Str<"Cycles">;
			}
			break;
		case NMib::NProcess::EProcessStatUnit_Seconds:
			{
				if (m_IdealScale == fp64(0.000000001))
				{
					Value /= m_IdealScale;
					o_Unit = NStr::gc_Str<"ns">;
				}
				else if (m_IdealScale == fp64(0.000001))
				{
					Value /= m_IdealScale;
					o_Unit = NStr::gc_Str<"µs">;
				}
				else if (m_IdealScale == fp64(0.001))
				{
					Value /= m_IdealScale;
					o_Unit = NStr::gc_Str<"ms">;
				}
				else
					o_Unit = NStr::gc_Str<"s">;
			}
			break;
		case NMib::NProcess::EProcessStatUnit_Fraction:
			{
				Value *= 100.0;
				o_Unit = NStr::gc_Str<"%">;
			}
			break;
		default:
			DMibNeverGetHere(m_Unit);
			break;
		}
		return Value;
	}
}
