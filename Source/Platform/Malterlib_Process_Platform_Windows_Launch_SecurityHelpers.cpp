// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Core/Core>
#include <AclAPI.h>
#include <Windows.h>

#define DDesktopAll (DESKTOP_READOBJECTS | DESKTOP_CREATEWINDOW | \
	DESKTOP_CREATEMENU | DESKTOP_HOOKCONTROL | DESKTOP_JOURNALRECORD | \
	DESKTOP_JOURNALPLAYBACK | DESKTOP_ENUMERATE | DESKTOP_WRITEOBJECTS | \
	DESKTOP_SWITCHDESKTOP | STANDARD_RIGHTS_REQUIRED)

#define DWinStaAll (WINSTA_ENUMDESKTOPS | WINSTA_READATTRIBUTES | \
	WINSTA_ACCESSCLIPBOARD | WINSTA_CREATEDESKTOP | \
	WINSTA_WRITEATTRIBUTES | WINSTA_ACCESSGLOBALATOMS | \
	WINSTA_EXITWINDOWS | WINSTA_ENUMERATE | WINSTA_READSCREEN | \
	STANDARD_RIGHTS_REQUIRED)

#define DGenericAccess (GENERIC_READ | GENERIC_WRITE | \
	GENERIC_EXECUTE | GENERIC_ALL)

namespace NMib::NProcess::NPlatform
{
	BOOL fg_ChangeAceToWindowStation(HWINSTA _WindowStation, PSID _pSid, bool _bRemove, bool &_bAdded)
	{
		ACCESS_ALLOWED_ACE *pAce = NULL;
		ACL_SIZE_INFORMATION AclSizeInfo;
		BOOL bDaclExist;
		BOOL bDaclPresent;
		BOOL bSuccess = FALSE;
		DWORD NewAclSize;
		DWORD SidSize = 0;
		DWORD SdSizeNeeded;
		PACL pAcl;
		PACL pNewAcl = NULL;
		PSECURITY_DESCRIPTOR pSecurityDescriptor = NULL;
		PSECURITY_DESCRIPTOR pNewSecurityDescriptor = NULL;
		PVOID pTempAce;
		SECURITY_INFORMATION SecurityInformation = DACL_SECURITY_INFORMATION;
		unsigned int iAce;
		_bAdded = false;

		__try
		{
			// Obtain the DACL for the window station.

			if (!GetUserObjectSecurity(_WindowStation, &SecurityInformation, pSecurityDescriptor, SidSize, &SdSizeNeeded))
			{
				if (GetLastError() == ERROR_INSUFFICIENT_BUFFER)
				{
					pSecurityDescriptor = (PSECURITY_DESCRIPTOR)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, SdSizeNeeded);

					if (pSecurityDescriptor == NULL)
						__leave;

					pNewSecurityDescriptor = (PSECURITY_DESCRIPTOR)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, SdSizeNeeded);

					if (pNewSecurityDescriptor == NULL)
						__leave;

					SidSize = SdSizeNeeded;

					if (!GetUserObjectSecurity(_WindowStation, &SecurityInformation, pSecurityDescriptor, SidSize, &SdSizeNeeded))
						__leave;
				}
				else
					__leave;
			}

			// Create a new DACL.

			if (!InitializeSecurityDescriptor(pNewSecurityDescriptor, SECURITY_DESCRIPTOR_REVISION))
				__leave;

			// Get the DACL from the security descriptor.

			if (!GetSecurityDescriptorDacl(pSecurityDescriptor, &bDaclPresent, &pAcl, &bDaclExist))
				__leave;

			// Initialize the ACL.

			ZeroMemory(&AclSizeInfo, sizeof(ACL_SIZE_INFORMATION));
			AclSizeInfo.AclBytesInUse = sizeof(ACL);

			// Call only if the DACL is not NULL.

			if (pAcl != NULL)
			{
				// get the file ACL size info
				if (!GetAclInformation(pAcl, (LPVOID)&AclSizeInfo, sizeof(ACL_SIZE_INFORMATION), AclSizeInformation))
					__leave;
			}

			// Compute the size of the new ACL.

			NewAclSize = AclSizeInfo.AclBytesInUse + (2 * sizeof(ACCESS_ALLOWED_ACE)) + (2 * GetLengthSid(_pSid)) - (2 * sizeof(DWORD));

			// Allocate memory for the new ACL.

			pNewAcl = (PACL)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, NewAclSize);

			if (pNewAcl == NULL)
				__leave;

			// Initialize the new DACL.

			if (!InitializeAcl(pNewAcl, NewAclSize, ACL_REVISION))
				__leave;

			// If DACL is present, copy it to a new DACL.

			_bAdded = true;
			if (bDaclPresent)
			{
				// Copy the ACEs to the new ACL.
				if (AclSizeInfo.AceCount)
				{
					for (iAce = 0; iAce < AclSizeInfo.AceCount; iAce++)
					{
						// Get an ACE.
						if (!GetAce(pAcl, iAce, &pTempAce))
							__leave;

						ACCESS_ALLOWED_ACE &TempAce = *((ACCESS_ALLOWED_ACE *)pTempAce);
						if (TempAce.Header.AceType == ACCESS_ALLOWED_ACE_TYPE && EqualSid(&TempAce.SidStart, _pSid))
						{
							if (TempAce.Header.AceFlags == (CONTAINER_INHERIT_ACE | INHERIT_ONLY_ACE | OBJECT_INHERIT_ACE) && TempAce.Mask == DGenericAccess)
							{
								_bAdded = false;
								continue;
							}
							if (TempAce.Header.AceFlags == NO_PROPAGATE_INHERIT_ACE && TempAce.Mask == DWinStaAll)
							{
								_bAdded = false;
								continue;
							}
						}

						// Add the ACE to the new ACL.
						if (!AddAce(pNewAcl, ACL_REVISION, MAXDWORD, pTempAce, ((PACE_HEADER)pTempAce)->AceSize))
							__leave;
					}
				}
			}

			// Add the first ACE to the window station.

			if (!_bRemove)
			{
				pAce = (ACCESS_ALLOWED_ACE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(ACCESS_ALLOWED_ACE) + GetLengthSid(_pSid) - sizeof(DWORD));

				if (pAce == NULL)
					__leave;

				pAce->Header.AceType = ACCESS_ALLOWED_ACE_TYPE;
				pAce->Header.AceFlags = CONTAINER_INHERIT_ACE | INHERIT_ONLY_ACE | OBJECT_INHERIT_ACE;
				pAce->Header.AceSize = LOWORD(sizeof(ACCESS_ALLOWED_ACE) + GetLengthSid(_pSid) - sizeof(DWORD));
				pAce->Mask = DGenericAccess;

				if (!CopySid(GetLengthSid(_pSid), &pAce->SidStart, _pSid))
					__leave;

				if (!AddAce(pNewAcl, ACL_REVISION, MAXDWORD, (LPVOID)pAce, pAce->Header.AceSize))
					__leave;

				// Add the second ACE to the window station.

				pAce->Header.AceFlags = NO_PROPAGATE_INHERIT_ACE;
				pAce->Mask = DWinStaAll;

				if (!AddAce(pNewAcl, ACL_REVISION, MAXDWORD, (LPVOID)pAce, pAce->Header.AceSize))
					__leave;
			}

			// Set a new DACL for the security descriptor.

			if (!SetSecurityDescriptorDacl(pNewSecurityDescriptor, TRUE, pNewAcl, FALSE))
				__leave;

			// Set the new security descriptor for the window station.

			if (!SetUserObjectSecurity(_WindowStation, &SecurityInformation, pNewSecurityDescriptor))
				__leave;

			// Indicate success.

			bSuccess = TRUE;
		}
		__finally
		{
			// Free the allocated buffers.

			auto Error = GetLastError();

			if (pAce != NULL)
				HeapFree(GetProcessHeap(), 0, (LPVOID)pAce);

			if (pNewAcl != NULL)
				HeapFree(GetProcessHeap(), 0, (LPVOID)pNewAcl);

			if (pSecurityDescriptor != NULL)
				HeapFree(GetProcessHeap(), 0, (LPVOID)pSecurityDescriptor);

			if (pNewSecurityDescriptor != NULL)
				HeapFree(GetProcessHeap(), 0, (LPVOID)pNewSecurityDescriptor);

			SetLastError(Error);
		}

		return bSuccess;
	}

	BOOL fg_ChangeAceToDesktop(HDESK _Desktop, PSID _pSid, bool _bRemove, bool &_bAdded)
	{
		ACL_SIZE_INFORMATION AclSizeInfo;
		BOOL bDaclExist;
		BOOL bDaclPresent;
		BOOL bSuccess = FALSE;
		DWORD NewAclSize;
		DWORD SidSize = 0;
		DWORD SdSizeNeeded;
		PACL pAcl;
		PACL pNewAcl = NULL;
		PSECURITY_DESCRIPTOR pSecurityDescriptor = NULL;
		PSECURITY_DESCRIPTOR pNewSecurityDescriptor = NULL;
		PVOID pTempAce;
		SECURITY_INFORMATION SecurityInformation = DACL_SECURITY_INFORMATION;
		unsigned int iAce;
		_bAdded = false;

		__try
		{
			// Obtain the security descriptor for the desktop object.

			if (!GetUserObjectSecurity(_Desktop, &SecurityInformation, pSecurityDescriptor, SidSize, &SdSizeNeeded))
			{
				if (GetLastError() == ERROR_INSUFFICIENT_BUFFER)
				{
					pSecurityDescriptor = (PSECURITY_DESCRIPTOR)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, SdSizeNeeded);

					if (pSecurityDescriptor == NULL)
						__leave;

					pNewSecurityDescriptor = (PSECURITY_DESCRIPTOR)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, SdSizeNeeded);

					if (pNewSecurityDescriptor == NULL)
						__leave;

					SidSize = SdSizeNeeded;

					if (!GetUserObjectSecurity(_Desktop, &SecurityInformation, pSecurityDescriptor, SidSize, &SdSizeNeeded))
						__leave;
				}
				else
					__leave;
			}

			// Create a new security descriptor.

			if (!InitializeSecurityDescriptor(pNewSecurityDescriptor, SECURITY_DESCRIPTOR_REVISION))
				__leave;

			// Obtain the DACL from the security descriptor.

			if (!GetSecurityDescriptorDacl(pSecurityDescriptor, &bDaclPresent, &pAcl, &bDaclExist))
				__leave;

			// Initialize.

			ZeroMemory(&AclSizeInfo, sizeof(ACL_SIZE_INFORMATION));
			AclSizeInfo.AclBytesInUse = sizeof(ACL);

			// Call only if NULL DACL.

			if (pAcl != NULL)
			{
				// Determine the size of the ACL information.

				if (!GetAclInformation(pAcl, (LPVOID)&AclSizeInfo, sizeof(ACL_SIZE_INFORMATION), AclSizeInformation))
					__leave;
			}

			// Compute the size of the new ACL.

			NewAclSize = AclSizeInfo.AclBytesInUse + sizeof(ACCESS_ALLOWED_ACE) + GetLengthSid(_pSid) - sizeof(DWORD);

			// Allocate buffer for the new ACL.

			pNewAcl = (PACL)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, NewAclSize);

			if (pNewAcl == NULL)
				__leave;

			// Initialize the new ACL.

			if (!InitializeAcl(pNewAcl, NewAclSize, ACL_REVISION))
				__leave;

			// If DACL is present, copy it to a new DACL.

			_bAdded = true;

			if (bDaclPresent)
			{
				// Copy the ACEs to the new ACL.
				if (AclSizeInfo.AceCount)
				{
					for (iAce = 0; iAce < AclSizeInfo.AceCount; iAce++)
					{
						// Get an ACE.
						if (!GetAce(pAcl, iAce, &pTempAce))
							__leave;

						ACCESS_ALLOWED_ACE &TempAce = *((ACCESS_ALLOWED_ACE *)pTempAce);
						if (TempAce.Header.AceType == ACCESS_ALLOWED_ACE_TYPE && EqualSid(&TempAce.SidStart, _pSid))
						{
							if (TempAce.Header.AceFlags == 0 && TempAce.Mask == DDesktopAll)
							{
								_bAdded = false;
								continue;
							}
						}

						// Add the ACE to the new ACL.
						if (!AddAce(pNewAcl, ACL_REVISION, MAXDWORD, pTempAce, ((PACE_HEADER)pTempAce)->AceSize))
							__leave;
					}
				}
			}

			if (!_bRemove)
			{
				// Add ACE to the DACL.

				if (!AddAccessAllowedAce(pNewAcl, ACL_REVISION, DDesktopAll, _pSid))
					__leave;
			}

			// Set new DACL to the new security descriptor.

			if (!SetSecurityDescriptorDacl(pNewSecurityDescriptor, TRUE, pNewAcl, FALSE))
				__leave;

			// Set the new security descriptor for the desktop object.

			if (!SetUserObjectSecurity(_Desktop, &SecurityInformation, pNewSecurityDescriptor))
				__leave;

			// Indicate success.

			bSuccess = TRUE;
		}
		__finally
		{
			// Free buffers.

			auto Error = GetLastError();

			if (pNewAcl != NULL)
				HeapFree(GetProcessHeap(), 0, (LPVOID)pNewAcl);

			if (pSecurityDescriptor != NULL)
				HeapFree(GetProcessHeap(), 0, (LPVOID)pSecurityDescriptor);

			if (pNewSecurityDescriptor != NULL)
				HeapFree(GetProcessHeap(), 0, (LPVOID)pNewSecurityDescriptor);

			SetLastError(Error);
		}

		return bSuccess;
	}
}

