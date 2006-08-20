/**
*  MemReader.cpp
*  Copyright (C) 2005      nate
*  Copyright (C) 2006      bear
*
*  This file is part of TSFileSource, a directshow push source filter that
*  provides an MPEG transport stream output.
*
*  TSFileSource is free software; you can redistribute it and/or modify
*  it under the terms of the GNU General Public License as published by
*  the Free Software Foundation; either version 2 of the License, or
*  (at your option) any later version.
*
*  TSFileSource is distributed in the hope that it will be useful,
*  but WITHOUT ANY WARRANTY; without even the implied warranty of
*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*  GNU General Public License for more details.
*
*  You should have received a copy of the GNU General Public License
*  along with TSFileSource; if not, write to the Free Software
*  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*
*  nate can be reached on the forums at
*    http://forums.dvbowners.com/
*/

#include <streams.h>
#include "MemReader.h"
#include "global.h"

MemReader::MemReader(SharedMemory* pSharedMemory) :
	FileReader(pSharedMemory),
	m_pSharedMemory(pSharedMemory), 
	m_hFile(INVALID_HANDLE_VALUE),
	m_pFileName(0),
	m_bReadOnly(FALSE),
	m_fileSize(0),
	m_infoFileSize(0),
	m_fileStartPos(0),
	m_hInfoFile(INVALID_HANDLE_VALUE),
	m_bDelay(FALSE),
	m_bDebugOutput(FALSE)
{
}

MemReader::~MemReader()
{
	CloseFile();
	if (m_pFileName)
		delete m_pFileName;
}

FileReader* MemReader::CreateFileReader()
{
	return (FileReader*) new MemReader(m_pSharedMemory);
}

HRESULT MemReader::GetFileName(LPOLESTR *lpszFileName)
{
	*lpszFileName = m_pFileName;
	return S_OK;
}

HRESULT MemReader::SetFileName(LPCOLESTR pszFileName)
{
	// Is this a valid filename supplied
	CheckPointer(pszFileName,E_POINTER);

	if(wcslen(pszFileName) > MAX_PATH)
		return ERROR_FILENAME_EXCED_RANGE;

	// Take a copy of the filename

	if (m_pFileName)
	{
		delete[] m_pFileName;
		m_pFileName = NULL;
	}
	m_pFileName = new WCHAR[1+lstrlenW(pszFileName)];
	if (m_pFileName == NULL)
		return E_OUTOFMEMORY;

	lstrcpyW(m_pFileName, pszFileName);

	return S_OK;
}

//
// OpenFile
//
// Opens the file ready for streaming
//
HRESULT MemReader::OpenFile()
{
	TCHAR *pFileName = NULL;

	// Is the file already opened
	if (m_hFile != INVALID_HANDLE_VALUE) {

		return NOERROR;
	}

	// Has a filename been set yet
	if (m_pFileName == NULL) {
		return ERROR_INVALID_NAME;
	}

//	BoostThread Boost;

	// Convert the UNICODE filename if necessary

#if defined(WIN32) && !defined(UNICODE)
	char convert[MAX_PATH];

	if(!WideCharToMultiByte(CP_ACP,0,m_pFileName,-1,convert,MAX_PATH,0,0))
		return ERROR_INVALID_NAME;

	pFileName = convert;
#else
	pFileName = m_pFileName;
#endif

	m_bReadOnly = FALSE;

	// Try to open the file
	m_hFile = m_pSharedMemory->CreateFile((LPCTSTR) pFileName,   // The filename
						 (DWORD) GENERIC_READ,          // File access
						 (DWORD) FILE_SHARE_READ,       // Share access
						 NULL,                  // Security
						 (DWORD) OPEN_EXISTING,         // Open flags
						 (DWORD) 0,             // More flags
						 NULL);                 // Template

	if (m_hFile == INVALID_HANDLE_VALUE) {

		//Test incase file is being recorded to
		m_hFile = m_pSharedMemory->CreateFile((LPCTSTR) pFileName,		// The filename
							(DWORD) GENERIC_READ,				// File access
							(DWORD) (FILE_SHARE_READ |
							FILE_SHARE_WRITE),   // Share access
							NULL,						// Security
							(DWORD) OPEN_EXISTING,				// Open flags
//							(DWORD) 0,
							(DWORD) FILE_ATTRIBUTE_NORMAL,		// More flags
//							FILE_ATTRIBUTE_NORMAL |
//							FILE_FLAG_RANDOM_ACCESS,	// More flags
//							FILE_FLAG_SEQUENTIAL_SCAN,	// More flags
							NULL);						// Template

		if (m_hFile == INVALID_HANDLE_VALUE)
		{
			DWORD dwErr = GetLastError();
			return HRESULT_FROM_WIN32(dwErr);
		}

		m_bReadOnly = TRUE;
	}

	TCHAR infoName[512];
	strcpy(infoName, pFileName);
	strcat(infoName, ".info");

	m_hInfoFile = m_pSharedMemory->CreateFile((LPCTSTR) infoName, // The filename
			(DWORD) GENERIC_READ,    // File access
			(DWORD) (FILE_SHARE_READ |
			FILE_SHARE_WRITE),   // Share access
			NULL,      // Security
			(DWORD) OPEN_EXISTING,    // Open flags
//			(DWORD) 0,
			(DWORD) FILE_ATTRIBUTE_NORMAL, // More flags
//			FILE_FLAG_SEQUENTIAL_SCAN,	// More flags
//			FILE_ATTRIBUTE_NORMAL |
//			FILE_FLAG_RANDOM_ACCESS,	// More flags
			NULL);

	SetFilePointer(0, FILE_BEGIN);

	return S_OK;

} // Open

//
// CloseFile
//
// Closes any dump file we have opened
//
HRESULT MemReader::CloseFile()
{
	// Must lock this section to prevent problems related to
	// closing the file while still receiving data in Receive()

	if (m_hFile == INVALID_HANDLE_VALUE) {

		return S_OK;
	}

//	BoostThread Boost;

	m_pSharedMemory->CloseHandle(m_hFile);
	m_hFile = INVALID_HANDLE_VALUE; // Invalidate the file

	if (m_hInfoFile != INVALID_HANDLE_VALUE)
		m_pSharedMemory->CloseHandle(m_hInfoFile);

	m_hInfoFile = INVALID_HANDLE_VALUE; // Invalidate the file

	return NOERROR;

} // CloseFile

BOOL MemReader::IsFileInvalid()
{
	return (m_hFile == INVALID_HANDLE_VALUE);
}

HRESULT MemReader::GetFileSize(__int64 *pStartPosition, __int64 *pLength)
{
	CheckPointer(pStartPosition,E_POINTER);
	CheckPointer(pLength,E_POINTER);
	
//	BoostThread Boost;

	GetStartPosition(pStartPosition);

	//Do not get file size if static file or first time 
	if (m_bReadOnly || !m_fileSize) {
		
		if (m_hInfoFile != INVALID_HANDLE_VALUE)
		{
			__int64 length = -1;
			DWORD read = 0;
			LARGE_INTEGER li;
			li.QuadPart = 0;
			m_pSharedMemory->SetFilePointer(m_hInfoFile, li.LowPart, &li.HighPart, FILE_BEGIN);
			m_pSharedMemory->ReadFile(m_hInfoFile, (PVOID)&length, (DWORD)sizeof(__int64), &read, NULL);

			if(length > -1)
			{
				m_fileSize = length;
				*pLength = length;
				return S_OK;
			}
		}


		DWORD dwSizeLow;
		DWORD dwSizeHigh;

		dwSizeLow = m_pSharedMemory->GetFileSize(m_hFile, &dwSizeHigh);
		if ((dwSizeLow == 0xFFFFFFFF) && (GetLastError() != NO_ERROR ))
		{
			return E_FAIL;
		}

		LARGE_INTEGER li;
		li.LowPart = dwSizeLow;
		li.HighPart = dwSizeHigh;
		m_fileSize = li.QuadPart;
	}
	*pLength = m_fileSize;
	return S_OK;
}

HRESULT MemReader::GetInfoFileSize(__int64 *lpllsize)
{
	//Do not get file size if static file or first time 
	if (m_bReadOnly || !m_infoFileSize) {
		
		DWORD dwSizeLow;
		DWORD dwSizeHigh;

//		BoostThread Boost;

		dwSizeLow = m_pSharedMemory->GetFileSize(m_hInfoFile, &dwSizeHigh);
		if ((dwSizeLow == 0xFFFFFFFF) && (GetLastError() != NO_ERROR ))
		{
			return E_FAIL;
		}

		LARGE_INTEGER li;
		li.LowPart = dwSizeLow;
		li.HighPart = dwSizeHigh;
		m_infoFileSize = li.QuadPart;

	}
		*lpllsize = m_infoFileSize;
		return S_OK;
}

HRESULT MemReader::GetStartPosition(__int64 *lpllpos)
{
	//Do not get file size if static file unless first time 
	if (m_bReadOnly || !m_fileStartPos) {
		
		if (m_hInfoFile != INVALID_HANDLE_VALUE)
		{
//			BoostThread Boost;
	
			__int64 size = 0;
			GetInfoFileSize(&size);
			//Check if timeshift info file
			if (size > sizeof(__int64))
			{
				//Get the file start pointer
				__int64 length = -1;
				DWORD read = 0;
				LARGE_INTEGER li;
				li.QuadPart = sizeof(__int64);
				m_pSharedMemory->SetFilePointer(m_hInfoFile, li.LowPart, &li.HighPart, FILE_BEGIN);
				m_pSharedMemory->ReadFile(m_hInfoFile, (PVOID)&length, (DWORD)sizeof(__int64), &read, NULL);

				if(length > -1)
				{
					m_fileStartPos = length;
					*lpllpos =  length;
					return S_OK;
				}
			}
		}
		m_fileStartPos = 0;
	}
	*lpllpos = m_fileStartPos;
	return S_OK;
}

DWORD MemReader::SetFilePointer(__int64 llDistanceToMove, DWORD dwMoveMethod)
{
//	BoostThread Boost;

	LARGE_INTEGER li;

	if (dwMoveMethod == FILE_END && m_hInfoFile != INVALID_HANDLE_VALUE)
	{
		__int64 startPos = 0;
		GetStartPosition(&startPos);

		if (startPos > 0)
		{
			__int64 start;
			__int64 fileSize = 0;
			GetFileSize(&start, &fileSize);

			__int64 filePos  = (__int64)((__int64)fileSize + (__int64)llDistanceToMove + (__int64)startPos);

			if (filePos >= fileSize)
				li.QuadPart = (__int64)((__int64)startPos + (__int64)llDistanceToMove);
			else
				li.QuadPart = filePos;

			return m_pSharedMemory->SetFilePointer(m_hFile, li.LowPart, &li.HighPart, FILE_BEGIN);
		}

		__int64 start = 0;
		__int64 length = 0;
		GetFileSize(&start, &length);

		length  = (__int64)((__int64)length + (__int64)llDistanceToMove);

		li.QuadPart = length;

		dwMoveMethod = FILE_BEGIN;
	}
	else
	{
		__int64 startPos = 0;
		GetStartPosition(&startPos);

		if (startPos > 0)
		{
			__int64 start;
			__int64 fileSize = 0;
			GetFileSize(&start, &fileSize);

			__int64 filePos  = (__int64)((__int64)startPos + (__int64)llDistanceToMove);

			if (filePos >= fileSize)
				li.QuadPart = (__int64)((__int64)filePos - (__int64)fileSize);
			else
				li.QuadPart = filePos;

			return m_pSharedMemory->SetFilePointer(m_hFile, li.LowPart, &li.HighPart, dwMoveMethod);
		}
		li.QuadPart = llDistanceToMove;
	}

	return m_pSharedMemory->SetFilePointer(m_hFile, li.LowPart, &li.HighPart, dwMoveMethod);
}

__int64 MemReader::GetFilePointer()
{
//	BoostThread Boost;

	LARGE_INTEGER li;
	li.QuadPart = 0;
	li.LowPart = m_pSharedMemory->SetFilePointer(m_hFile, 0, &li.HighPart, FILE_CURRENT);

	__int64 start;
	__int64 length = 0;
	GetFileSize(&start, &length);

	__int64 startPos = 0;
	GetStartPosition(&startPos);

	if (startPos > 0)
	{
		if(startPos > (__int64)li.QuadPart)
			li.QuadPart = (__int64)(length - startPos + (__int64)li.QuadPart);
		else
			li.QuadPart = (__int64)((__int64)li.QuadPart - startPos);
	}

	return li.QuadPart;
}

HRESULT MemReader::Read(PBYTE pbData, ULONG lDataLength, ULONG *dwReadBytes)
{
	HRESULT hr;

	// If the file has already been closed, don't continue
	if (m_hFile == INVALID_HANDLE_VALUE)
		return S_FALSE;

//	BoostThread Boost;

	//Get File Position
	LARGE_INTEGER li;
	li.QuadPart = 0;
	li.LowPart = m_pSharedMemory->SetFilePointer(m_hFile, 0, &li.HighPart, FILE_CURRENT);
	__int64 m_filecurrent = li.QuadPart;

	if (m_hInfoFile != INVALID_HANDLE_VALUE)
	{
		__int64 startPos = 0;
		GetStartPosition(&startPos);

		if (startPos > 0)
		{
			__int64 start;
			__int64 length = 0;
			GetFileSize(&start, &length);

			if (length < (__int64)(m_filecurrent + (__int64)lDataLength) && m_filecurrent > startPos)
			{

				hr = m_pSharedMemory->ReadFile(m_hFile, (PVOID)pbData, (DWORD)(length - m_filecurrent), dwReadBytes, NULL);
				if (FAILED(hr))
					return hr;

				LARGE_INTEGER li;
				li.QuadPart = 0;
				m_pSharedMemory->SetFilePointer(m_hFile, li.LowPart, &li.HighPart, FILE_BEGIN);

				ULONG dwRead = 0;

				hr = m_pSharedMemory->ReadFile(m_hFile,
					(PVOID)(pbData + (DWORD)(length - m_filecurrent)),
					(DWORD)((__int64)lDataLength -(__int64)((__int64)length - (__int64)m_filecurrent)),
					&dwRead,
					NULL);

				*dwReadBytes = *dwReadBytes + dwRead;

			}
			else if (startPos < (__int64)(m_filecurrent + (__int64)lDataLength) && m_filecurrent < startPos)
				hr = m_pSharedMemory->ReadFile(m_hFile, (PVOID)pbData, (DWORD)(startPos - m_filecurrent), dwReadBytes, NULL);

			else
				hr = m_pSharedMemory->ReadFile(m_hFile, (PVOID)pbData, (DWORD)lDataLength, dwReadBytes, NULL);

			if (FAILED(hr))
				return hr;

			if (*dwReadBytes < (ULONG)lDataLength)
				return S_FALSE;

			return S_OK;
		}

		__int64 start = 0;
		__int64 length = 0;
		GetFileSize(&start, &length);
		if (length < (__int64)(m_filecurrent + (__int64)lDataLength))
			hr = m_pSharedMemory->ReadFile(m_hFile, (PVOID)pbData, (DWORD)(length - m_filecurrent), dwReadBytes, NULL);
		else
			hr = m_pSharedMemory->ReadFile(m_hFile, (PVOID)pbData, (DWORD)lDataLength, dwReadBytes, NULL);
	}
	else
		hr = m_pSharedMemory->ReadFile(m_hFile, (PVOID)pbData, (DWORD)lDataLength, dwReadBytes, NULL);//Read file data into buffer

	if (FAILED(hr))
		return hr;
	if (*dwReadBytes < (ULONG)lDataLength)
		return S_FALSE;

	return S_OK;
}

HRESULT MemReader::Read(PBYTE pbData, ULONG lDataLength, ULONG *dwReadBytes, __int64 llDistanceToMove, DWORD dwMoveMethod)
{
//	BoostThread Boost;

	//If end method then we want llDistanceToMove to be the end of the buffer that we read.
	if (dwMoveMethod == FILE_END)
		llDistanceToMove = 0 - llDistanceToMove - lDataLength;

	SetFilePointer(llDistanceToMove, dwMoveMethod);

	return Read(pbData, lDataLength, dwReadBytes);
}

HRESULT MemReader::get_ReadOnly(WORD *ReadOnly)
{
	*ReadOnly = m_bReadOnly;
	return S_OK;
}

HRESULT MemReader::get_DelayMode(WORD *DelayMode)
{
	*DelayMode = m_bDelay;
	return S_OK;
}

HRESULT MemReader::set_DelayMode(WORD DelayMode)
{
	m_bDelay = DelayMode;
	return S_OK;
}

HRESULT MemReader::get_ReaderMode(WORD *ReaderMode)
{
	*ReaderMode = FALSE;
	return S_OK;
}

void MemReader::SetDebugOutput(BOOL bDebugOutput)
{
	m_bDebugOutput = bDebugOutput;
}

DWORD MemReader::setFilePointer(__int64 llDistanceToMove, DWORD dwMoveMethod)
{
	//Get the file information
	__int64 fileStart, fileEnd, fileLength;
	GetFileSize(&fileStart, &fileLength);
	fileEnd = fileLength;
	if (dwMoveMethod == FILE_BEGIN)
		return SetFilePointer((__int64)min(fileEnd, llDistanceToMove), FILE_BEGIN);
	else
		return SetFilePointer((__int64)max((__int64)-fileLength, llDistanceToMove), FILE_END);
}

__int64 MemReader::getFilePointer()
{
	return GetFilePointer();
}

