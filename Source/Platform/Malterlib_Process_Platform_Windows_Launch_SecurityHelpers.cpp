// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Core/Core>
#include <AclAPI.h>
#include <Windows.h>

#define DESKTOP_ALL (DESKTOP_READOBJECTS | DESKTOP_CREATEWINDOW | \
DESKTOP_CREATEMENU | DESKTOP_HOOKCONTROL | DESKTOP_JOURNALRECORD | \
DESKTOP_JOURNALPLAYBACK | DESKTOP_ENUMERATE | DESKTOP_WRITEOBJECTS | \
DESKTOP_SWITCHDESKTOP | STANDARD_RIGHTS_REQUIRED)

#define WINSTA_ALL (WINSTA_ENUMDESKTOPS | WINSTA_READATTRIBUTES | \
WINSTA_ACCESSCLIPBOARD | WINSTA_CREATEDESKTOP | \
WINSTA_WRITEATTRIBUTES | WINSTA_ACCESSGLOBALATOMS | \
WINSTA_EXITWINDOWS | WINSTA_ENUMERATE | WINSTA_READSCREEN | \
STANDARD_RIGHTS_REQUIRED)

#define GENERIC_ACCESS (GENERIC_READ | GENERIC_WRITE | \
GENERIC_EXECUTE | GENERIC_ALL)

namespace NMib::NProcess::NPlatform
{
	BOOL fg_ChangeAceToWindowStation(HWINSTA hwinsta, PSID psid, bool _bRemove, bool &_bAdded)
	{
		ACCESS_ALLOWED_ACE   *pace = NULL;
		ACL_SIZE_INFORMATION aclSizeInfo;
		BOOL                 bDaclExist;
		BOOL                 bDaclPresent;
		BOOL                 bSuccess = FALSE;
		DWORD                dwNewAclSize;
		DWORD                dwSidSize = 0;
		DWORD                dwSdSizeNeeded;
		PACL                 pacl;
		PACL                 pNewAcl = NULL;
		PSECURITY_DESCRIPTOR psd = NULL;
		PSECURITY_DESCRIPTOR psdNew = NULL;
		PVOID                pTempAce;
		SECURITY_INFORMATION si = DACL_SECURITY_INFORMATION;
		unsigned int         i;
		_bAdded = false;

		__try
		{
			// Obtain the DACL for the window station.

			if (
					!GetUserObjectSecurity
					(
						hwinsta
						, &si
						, psd
						, dwSidSize
						, &dwSdSizeNeeded
					)
				)
			{
				if (GetLastError() == ERROR_INSUFFICIENT_BUFFER)
				{
					psd = (PSECURITY_DESCRIPTOR)HeapAlloc(
						GetProcessHeap(),
						HEAP_ZERO_MEMORY,
						dwSdSizeNeeded);

					if (psd == NULL)
						__leave;

					psdNew = (PSECURITY_DESCRIPTOR)HeapAlloc(
						GetProcessHeap(),
						HEAP_ZERO_MEMORY,
						dwSdSizeNeeded);

					if (psdNew == NULL)
						__leave;

					dwSidSize = dwSdSizeNeeded;

					if
						(
							!GetUserObjectSecurity
							(
								hwinsta,
								&si,
								psd,
								dwSidSize,
								&dwSdSizeNeeded
							)
						)
					{
						__leave;
					}
				}
				else
				{
					__leave;
				}
			}

			// Create a new DACL.

			if (!InitializeSecurityDescriptor(
				psdNew,
				SECURITY_DESCRIPTOR_REVISION)
				)
				__leave;

			// Get the DACL from the security descriptor.

			if (!GetSecurityDescriptorDacl(
				psd,
				&bDaclPresent,
				&pacl,
				&bDaclExist)
				)
				__leave;

			// Initialize the ACL.

			ZeroMemory(&aclSizeInfo, sizeof(ACL_SIZE_INFORMATION));
			aclSizeInfo.AclBytesInUse = sizeof(ACL);

			// Call only if the DACL is not NULL.

			if (pacl != NULL)
			{
				// get the file ACL size info
				if (!GetAclInformation(pacl, (LPVOID)&aclSizeInfo, sizeof(ACL_SIZE_INFORMATION), AclSizeInformation))
					__leave;
			}

			// Compute the size of the new ACL.

			dwNewAclSize = aclSizeInfo.AclBytesInUse +
				(2 * sizeof(ACCESS_ALLOWED_ACE)) + (2 * GetLengthSid(psid)) -
				(2 * sizeof(DWORD));

			// Allocate memory for the new ACL.

			pNewAcl = (PACL)HeapAlloc(
				GetProcessHeap(),
				HEAP_ZERO_MEMORY,
				dwNewAclSize);

			if (pNewAcl == NULL)
				__leave;

			// Initialize the new DACL.

			if (!InitializeAcl(pNewAcl, dwNewAclSize, ACL_REVISION))
				__leave;

			// If DACL is present, copy it to a new DACL.

			_bAdded = true;
			if (bDaclPresent)
			{
				// Copy the ACEs to the new ACL.
				if (aclSizeInfo.AceCount)
				{
					for (i = 0; i < aclSizeInfo.AceCount; i++)
					{
						// Get an ACE.
						if (!GetAce(pacl, i, &pTempAce))
							__leave;

						ACCESS_ALLOWED_ACE &TempAce = *((ACCESS_ALLOWED_ACE *)pTempAce);
						if (TempAce.Header.AceType == ACCESS_ALLOWED_ACE_TYPE && EqualSid(&TempAce.SidStart, psid))
						{
							if (TempAce.Header.AceFlags == (CONTAINER_INHERIT_ACE | INHERIT_ONLY_ACE | OBJECT_INHERIT_ACE) && TempAce.Mask == GENERIC_ACCESS)
							{
								_bAdded = false;
								continue;
							}
							if (TempAce.Header.AceFlags == NO_PROPAGATE_INHERIT_ACE && TempAce.Mask == WINSTA_ALL)
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
				pace = (ACCESS_ALLOWED_ACE *)HeapAlloc(
					GetProcessHeap(),
					HEAP_ZERO_MEMORY,
					sizeof(ACCESS_ALLOWED_ACE) + GetLengthSid(psid) -
					sizeof(DWORD));

				if (pace == NULL)
					__leave;

				pace->Header.AceType = ACCESS_ALLOWED_ACE_TYPE;
				pace->Header.AceFlags = CONTAINER_INHERIT_ACE | INHERIT_ONLY_ACE | OBJECT_INHERIT_ACE;
				pace->Header.AceSize = LOWORD(sizeof(ACCESS_ALLOWED_ACE) + GetLengthSid(psid) - sizeof(DWORD));
				pace->Mask = GENERIC_ACCESS;

				if (!CopySid(GetLengthSid(psid), &pace->SidStart, psid))
					__leave;

				if (!AddAce(pNewAcl, ACL_REVISION, MAXDWORD, (LPVOID)pace, pace->Header.AceSize))
					__leave;

				// Add the second ACE to the window station.

				pace->Header.AceFlags = NO_PROPAGATE_INHERIT_ACE;
				pace->Mask = WINSTA_ALL;

				if (!AddAce(pNewAcl, ACL_REVISION, MAXDWORD, (LPVOID)pace, pace->Header.AceSize))
					__leave;
			}

			// Set a new DACL for the security descriptor.

			if (!SetSecurityDescriptorDacl(psdNew, TRUE, pNewAcl, FALSE) )
				__leave;

			// Set the new security descriptor for the window station.

			if (!SetUserObjectSecurity(hwinsta, &si, psdNew))
				__leave;

			// Indicate success.

			bSuccess = TRUE;
		}
		__finally
		{
			// Free the allocated buffers.

			auto Error = GetLastError();

			if (pace != NULL)
				HeapFree(GetProcessHeap(), 0, (LPVOID)pace);

			if (pNewAcl != NULL)
				HeapFree(GetProcessHeap(), 0, (LPVOID)pNewAcl);

			if (psd != NULL)
				HeapFree(GetProcessHeap(), 0, (LPVOID)psd);

			if (psdNew != NULL)
				HeapFree(GetProcessHeap(), 0, (LPVOID)psdNew);

			SetLastError(Error);
		}

		return bSuccess;
	}

	BOOL fg_ChangeAceToDesktop(HDESK hdesk, PSID psid, bool _bRemove, bool &_bAdded)
	{
		ACL_SIZE_INFORMATION aclSizeInfo;
		BOOL                 bDaclExist;
		BOOL                 bDaclPresent;
		BOOL                 bSuccess = FALSE;
		DWORD                dwNewAclSize;
		DWORD                dwSidSize = 0;
		DWORD                dwSdSizeNeeded;
		PACL                 pacl;
		PACL                 pNewAcl = NULL;
		PSECURITY_DESCRIPTOR psd = NULL;
		PSECURITY_DESCRIPTOR psdNew = NULL;
		PVOID                pTempAce;
		SECURITY_INFORMATION si = DACL_SECURITY_INFORMATION;
		unsigned int         i;
		_bAdded = false;

		__try
		{
			// Obtain the security descriptor for the desktop object.

			if (!GetUserObjectSecurity(hdesk, &si, psd, dwSidSize, &dwSdSizeNeeded))
			{
				if (GetLastError() == ERROR_INSUFFICIENT_BUFFER)
				{
					psd = (PSECURITY_DESCRIPTOR)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, dwSdSizeNeeded);

					if (psd == NULL)
						__leave;

					psdNew = (PSECURITY_DESCRIPTOR)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, dwSdSizeNeeded);

					if (psdNew == NULL)
						__leave;

					dwSidSize = dwSdSizeNeeded;

					if (!GetUserObjectSecurity(hdesk, &si, psd, dwSidSize, &dwSdSizeNeeded))
						__leave;
				}
				else
					__leave;
			}

			// Create a new security descriptor.

			if (!InitializeSecurityDescriptor(psdNew, SECURITY_DESCRIPTOR_REVISION))
				__leave;

			// Obtain the DACL from the security descriptor.

			if (!GetSecurityDescriptorDacl(psd, &bDaclPresent, &pacl, &bDaclExist))
				__leave;

			// Initialize.

			ZeroMemory(&aclSizeInfo, sizeof(ACL_SIZE_INFORMATION));
			aclSizeInfo.AclBytesInUse = sizeof(ACL);

			// Call only if NULL DACL.

			if (pacl != NULL)
			{
				// Determine the size of the ACL information.

				if (!GetAclInformation(pacl, (LPVOID)&aclSizeInfo, sizeof(ACL_SIZE_INFORMATION), AclSizeInformation))
					__leave;
			}

			// Compute the size of the new ACL.

			dwNewAclSize = aclSizeInfo.AclBytesInUse + sizeof(ACCESS_ALLOWED_ACE) + GetLengthSid(psid) - sizeof(DWORD);

			// Allocate buffer for the new ACL.

			pNewAcl = (PACL)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, dwNewAclSize);

			if (pNewAcl == NULL)
				__leave;

			// Initialize the new ACL.

			if (!InitializeAcl(pNewAcl, dwNewAclSize, ACL_REVISION))
				__leave;

			// If DACL is present, copy it to a new DACL.

			_bAdded = true;

			if (bDaclPresent)
			{
				// Copy the ACEs to the new ACL.
				if (aclSizeInfo.AceCount)
				{
					for (i = 0; i < aclSizeInfo.AceCount; i++)
					{
						// Get an ACE.
						if (!GetAce(pacl, i, &pTempAce))
							__leave;

						ACCESS_ALLOWED_ACE &TempAce = *((ACCESS_ALLOWED_ACE *)pTempAce);
						if (TempAce.Header.AceType == ACCESS_ALLOWED_ACE_TYPE && EqualSid(&TempAce.SidStart, psid))
						{
							if (TempAce.Header.AceFlags == 0 && TempAce.Mask == DESKTOP_ALL)
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

				if (!AddAccessAllowedAce(pNewAcl, ACL_REVISION, DESKTOP_ALL, psid))
					__leave;
			}

			// Set new DACL to the new security descriptor.

			if (!SetSecurityDescriptorDacl(psdNew, TRUE, pNewAcl, FALSE))
				__leave;

			// Set the new security descriptor for the desktop object.

			if (!SetUserObjectSecurity(hdesk, &si, psdNew))
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

			if (psd != NULL)
				HeapFree(GetProcessHeap(), 0, (LPVOID)psd);

			if (psdNew != NULL)
				HeapFree(GetProcessHeap(), 0, (LPVOID)psdNew);

			SetLastError(Error);
		}

		return bSuccess;
	}
}

