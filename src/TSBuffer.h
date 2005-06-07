/**
*  TSBuffer.h
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

#ifndef TSBUFFER_H
#define TSBUFFER_H

#include <vector>
#include "FileReader.h"
#include "PidInfo.h"

class CTSBuffer
{
public:

//***********************************************************************************************
//Refresh additions

	CTSBuffer(FileReader *pFileReader, PidInfo *pPids, PidInfoArray *pPidArray);
//Removed	CTSBuffer(FileReader *pFileReader, PidInfo *pPids);

//***********************************************************************************************

	virtual ~CTSBuffer();

	void Clear();
	long Count();
	HRESULT Require(long nBytes);

	HRESULT DequeFromBuffer(BYTE *pbData, long lDataLength);
	HRESULT ReadFromBuffer(BYTE *pbData, long lDataLength, long lOffset);

protected:
	FileReader *m_pFileReader;
	PidInfo *m_pPids;

//***********************************************************************************************
//Refresh additions
	PidInfoArray *m_pPidArray;
//***********************************************************************************************

	std::vector<BYTE *> m_Array;
	long m_lItemOffset;

	long m_lTSBufferItemSize;
};

#endif