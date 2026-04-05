// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Core/Core>
#include "../Malterlib_Process_Platform.h"
#include "Malterlib_Process_Platform_POSIX.h"

#include <libelf.h>
#include <gelf.h>
#include <stddef.h>


namespace
{
	NMib::NStr::CFStr256 fg_FormatElfErrno(const ch8 *_pDesc, int _Err)
	{
		return NMib::NStr::CFStr256::CFormat("{}: {} Desc: {}") << _pDesc << _Err << elf_errmsg(_Err);
	}
}

void NMib::NProcess::NPlatform::fg_Process_GetVersionInfo(NMib::NStr::CStr const &_File, NMib::NProcess::CVersionInfo &_VersionInfo)
{
	_VersionInfo.m_Major = 0;
	_VersionInfo.m_Minor = 0;
	_VersionInfo.m_Revision = 0;
	_VersionInfo.m_MinorRevision = 0;

	// We need a static version of libelf as this library is not available on Debian Lenny default install

	if (elf_version(EV_CURRENT) == EV_NONE)
		DMibError(fg_FormatElfErrno("ELF library initialization failed", elf_errno()));

	elf_errno(); // Reset ELF error code

	NMib::NStr::CStr Path = _File;

	NMib::NFile::CFile File;

	File.f_Open(Path, NMib::NFile::EFileOpen_Read | NMib::NFile::EFileOpen_ShareAll);

	Elf * pElf = elf_begin((umint)File.f_GetOSFile(), ELF_C_READ, nullptr);

	if (!pElf)
		DMibError(fg_FormatElfErrno("elf_begin failed", elf_errno()));

	auto Cleanup
		= fg_OnScopeExit
		(
			[&]
			{
				elf_end(pElf);
			}
		 )
	;

	if (elf_kind(pElf) != ELF_K_ELF)
		DMibError(fg_FormatElfErrno("elf_kind failed, its not an ELF object", elf_errno()));

	size_t Shstrndx;

	if (elf_getshdrstrndx(pElf, &Shstrndx) != 0)
		DMibError(fg_FormatElfErrno("elf_getshdrstrndx failed", elf_errno()));

	GElf_Shdr SectionHeader;
	Elf_Scn * pElfScan = nullptr;
	char * pSectionName = nullptr;
	Elf_Data * pElfData = nullptr;

	while ((pElfScan = elf_nextscn(pElf, pElfScan)) != nullptr)
	{
		if (gelf_getshdr(pElfScan, &SectionHeader) != &SectionHeader)
			DMibError(fg_FormatElfErrno("gelf_getshdr failed", elf_errno()));

		pSectionName = elf_strptr(pElf, Shstrndx , SectionHeader.sh_name);

		if (pSectionName == nullptr)
			DMibError(fg_FormatElfErrno("elf_strptr failed", elf_errno()));

		// This needs to be named exactly this to be compatible with old versions (Malterlib was previously named Ids)
		if (NMib::NStr::fg_StrCmp(pSectionName, "IDSVERSIONINFO") == 0)
		{
			pElfData = elf_getdata(pElfScan, pElfData);

			break;
		}
	}

	if (!pElfData)
		DMibError("No elf data for IDSVERSIONINFO found when scanning headers with libelf");

	NMib::NStr::CStr VersionInfo = (char *)pElfData->d_buf;

	NMib::NStr::CStr VersionInfoString = VersionInfo;

	while (!VersionInfoString.f_IsEmpty())
	{
		NMib::NStr::CStr Line = fg_GetStrLineSep(VersionInfoString);
		NMib::NStr::CStr Var = fg_GetStrSep(Line, "=");

		if (Var == "ProductVersionMajor")
			_VersionInfo.m_Major = Line.f_ToInt(int32(0));
		else if (Var == "ProductVersionMinor")
			_VersionInfo.m_Minor = Line.f_ToInt(int32(0));
		else if (Var == "ProductVersionRevision")
			_VersionInfo.m_Revision = Line.f_ToInt(int32(0));
		else if (Var == "BuildTime")
		{
			NMib::NStr::CStr BuildTime = Line;
			NMib::NStr::CStr BuildTimeSeconds = fg_GetStrSep(BuildTime, ":");
			NMib::NStr::CStr BuildTimeFraction = fg_GetStrSep(BuildTime, ":");

			if (!BuildTimeSeconds.f_IsEmpty())
				_VersionInfo.m_BuildTime = NMib::NTime::CTime::fs_Create(BuildTimeSeconds.f_ToInt(int64(0)), BuildTimeFraction.f_ToInt(uint64(0)));
		}
		else if (Var == "MalterlibBranch")
			_VersionInfo.m_Branch = Line;
		else if (Var == "MalterlibGitBranch")
			_VersionInfo.m_GitBranch = Line;
		else if (Var == "MalterlibGitCommit")
			_VersionInfo.m_GitCommit = Line;
	}
}
