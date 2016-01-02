// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include "../Malterlib_Process_Platform.h"

#pragma comment(lib, "Version.lib")
namespace
{
	uint32 fg_GetPEInfo(CStr _File, uint16 &_MajorVersion, uint16 &_MinorVersion)
	{
		if (!NFile::CFile::fs_FileExists(_File))
			return 0;
		uint32 TimeStamp = 0;
		NFile::CFile File;
		File.f_Open(_File, EFileOpen_Read | EFileOpen_ShareAll);
		uint32 Chunk[sizeof(uint32) * 256];
		CMibFilePos ToRead = File.f_GetLength() / 4;
		CMibFilePos FilePos = 0;
		while (ToRead)
		{
			mint ThisTime = fg_Min(ToRead, 256);
			File.f_Read(Chunk, ThisTime * sizeof(uint32));
			for (mint i = 0; i < ThisTime; ++i)
			{
				if (Chunk[i] == 'EP')
				{
					File.f_SetPosition(FilePos + i * sizeof(uint32));
					IMAGE_NT_HEADERS Headers;
					File.f_Read(&Headers, sizeof(Headers));

					if (Headers.FileHeader.Machine == IMAGE_FILE_MACHINE_I386)
					{
						File.f_SetPosition(FilePos + i * sizeof(uint32));
						IMAGE_NT_HEADERS32 Headers32;
						File.f_Read(&Headers32, sizeof(Headers32));

						_MajorVersion = Headers32.OptionalHeader.MajorImageVersion;
						_MinorVersion = Headers32.OptionalHeader.MinorImageVersion;
					}
					else if (Headers.FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64)
					{
						File.f_SetPosition(FilePos + i * sizeof(uint32));
						IMAGE_NT_HEADERS64 Headers64;
						File.f_Read(&Headers64, sizeof(Headers64));
						_MajorVersion = Headers64.OptionalHeader.MajorImageVersion;
						_MinorVersion = Headers64.OptionalHeader.MinorImageVersion;
					}

					return Headers.FileHeader.TimeDateStamp;
				}
			}
			ToRead -= ThisTime;
			FilePos += ThisTime;
		}
		return TimeStamp;
	}
}
void NMib::NProcess::NPlatform::fg_Process_GetVersionInfo(NMib::NStr::CStr const &_File, NMib::NProcess::CVersionInfo &_VersionInfo)
{
	CWStr File = fg_ConvertToWindowsPathLocal(_File);

	_VersionInfo.m_Major = 0;
	_VersionInfo.m_Minor = 0;
	_VersionInfo.m_Revision = 0;
	_VersionInfo.m_MinorRevision = 0;

	do 
	{
		DWORD Size = GetFileVersionInfoSizeW(File, nullptr);
		if (Size == 0)
			break;

		TCVector<uint8> Data;
		Data.f_SetLen(Size);
		if (!GetFileVersionInfoW(File, 0, Size, Data.f_GetArray()))
			break;

		void *pBlock = Data.f_GetArray();

		UINT QuerySize = 0;
		VS_FIXEDFILEINFO *pFixedFileInfo;

		if (!VerQueryValue(pBlock, str_utf16("\\"), (LPVOID*)&pFixedFileInfo, &QuerySize))
			break;


		_VersionInfo.m_Major = pFixedFileInfo->dwFileVersionMS >> 16;
		_VersionInfo.m_Minor = (pFixedFileInfo->dwFileVersionMS & 0xFFFF);
		_VersionInfo.m_Revision = pFixedFileInfo->dwFileVersionLS >> 16;
		_VersionInfo.m_MinorRevision = (pFixedFileInfo->dwFileVersionLS & 0xFFFF);


		struct LANGANDCODEPAGE
		{
			WORD wLanguage;
			WORD wCodePage;
		} *pTranslate;

		if (VerQueryValue(pBlock, str_utf16("\\VarFileInfo\\Translation"), (LPVOID*)&pTranslate, &QuerySize))
		{
			for (mint i = 0; i < (QuerySize/sizeof(struct LANGANDCODEPAGE)); ++i)
			{
				CWStr SubBlock = CWStr::CFormat(str_utf16("\\StringFileInfo\\{nfh,sj4,sf0,nc}{nfh,sj4,sf0,nc}\\PrivateBuild")) << pTranslate[i].wLanguage << pTranslate[i].wCodePage;

				ch16 const *pBuffer = nullptr;
				UINT BufferBytes = 0;
				if (VerQueryValue(pBlock, SubBlock.f_GetStr(), (LPVOID*)&pBuffer, &BufferBytes))
				{
					_VersionInfo.m_Branch = CWStr(pBuffer);
					break;
				}
			}
		}
	}
	while (false)
		;

	uint16 PEMajor = 0;
	uint16 PEMinor = 0;
	uint32 TimeStamp = fg_GetPEInfo(_File, PEMajor, PEMinor);
	if (PEMajor > _VersionInfo.m_Major)
	{
		_VersionInfo.m_Major = PEMajor;
		_VersionInfo.m_Minor = PEMinor / 1000;
		_VersionInfo.m_Revision = PEMinor % 1000;
	}

	_VersionInfo.m_BuildTime = CTimeConvert::fs_CreateTime(1970) + CTimeSpan((int64)TimeStamp);
}