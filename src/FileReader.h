/**
*  FileReader.h
*  Copyright (C) 2005      nate
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

#ifndef FILEREADER
#define FILEREADER

#include "PidInfo.h"

class FileReader
{
public:

	FileReader();
	virtual ~FileReader();

	// Open and write to the file
	HRESULT GetFileName(LPOLESTR *lpszFileName);
	HRESULT SetFileName(LPCOLESTR pszFileName);
	HRESULT OpenFile();
	HRESULT CloseFile();
	HRESULT Read(PBYTE pbData, ULONG lDataLength, ULONG *dwReadBytes);
	HRESULT Read(PBYTE pbData, ULONG lDataLength, ULONG *dwReadBytes, __int64 llDistanceToMove, DWORD dwMoveMethod);
	HRESULT get_ReadOnly(WORD *ReadOnly);
	HRESULT get_DelayMode(WORD *DelayMode);
	HRESULT set_DelayMode(WORD DelayMode);
	HRESULT GetFileSize(__int64 *lpllsize);

//*******************************************************************************************
//TimeShift Additions

	HRESULT GetInfoFileSize(__int64 *lpllsize);
	HRESULT GetStartPosition(__int64 *lpllpos);
	HRESULT get_TimeMode(WORD *TimeMode);

//*******************************************************************************************

	BOOL IsFileInvalid();

	DWORD SetFilePointer(__int64 llDistanceToMove, DWORD dwMoveMethod);
	__int64 GetFilePointer();
	__int64 get_FileSize(void);

protected:
	HANDLE   m_hFile; 				// Handle to file for streaming
	HANDLE   m_hInfoFile;           // Handle to Infofile for filesize from FileWriter
	LPOLESTR m_pFileName;           // The filename where we stream
	BOOL     m_bReadOnly;
	BOOL     m_bDelay;
	__int64 m_fileSize;

//*******************************************************************************************
//TimeShift Additions

	__int64 m_infoFileSize;
	__int64 m_fileStartPos;

//*******************************************************************************************

};

#endif
