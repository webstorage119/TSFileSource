/**
*  TSFileSource.cpp
*  Copyright (C) 2003      bisswanger
*  Copyright (C) 2004-2006 bear
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
*  bisswanger can be reached at WinSTB@hotmail.com
*    Homepage: http://www.winstb.de
*
*  bear and nate can be reached on the forums at
*    http://forums.dvbowners.com/
*/
#include <streams.h>

#include "bdaiface.h"
#include "ks.h"
#include "ksmedia.h"
#include "bdamedia.h"
#include "mediaformats.h"

#include "TSFileSource.h"
#include "TSFileSourceGuids.h"
#include "TunerEvent.h"
#include "global.h"


CUnknown * WINAPI CTSFileSourceFilter::CreateInstance(LPUNKNOWN punk, HRESULT *phr)
{
	ASSERT(phr);
	CTSFileSourceFilter *pNewObject = new CTSFileSourceFilter(punk, phr);

	if (pNewObject == NULL) {
		if (phr)
			*phr = E_OUTOFMEMORY;
	}

	return pNewObject;
}

// Constructor
CTSFileSourceFilter::CTSFileSourceFilter(IUnknown *pUnk, HRESULT *phr) :
	CSource(NAME("CTSFileSourceFilter"), pUnk, CLSID_TSFileSource),
	m_bRotEnable(FALSE),
	m_bSharedMode(FALSE),
	m_pPin(NULL),
	m_pDVBTChannels(NULL),
	m_WriteThreadActive(FALSE),
	m_FilterRefList(NAME("MyFilterRefList"))
{
	ASSERT(phr);

	m_pClock = new CTSFileSourceClock( NAME(""), GetOwner(), phr );
	if (m_pClock == NULL)
	{
		if (phr)
			*phr = E_OUTOFMEMORY;
		return;
	}

	m_pSharedMemory = new SharedMemory(64000000);
	m_pSampleBuffer = new CSampleBuffer();
	m_pFileReader = new FileReader(m_pSharedMemory);
	m_pFileDuration = new FileReader(m_pSharedMemory);//Get Live File Duration Thread
	m_pPidParser = new PidParser(m_pSampleBuffer, m_pFileReader);
	m_pDemux = new Demux(m_pPidParser, this, &m_FilterRefList);
	m_pStreamParser = new StreamParser(m_pPidParser, m_pDemux, &netArray);

	m_pMpeg2DataParser = NULL;
	m_pMpeg2DataParser = new DVBMpeg2DataParser();
	m_pDVBTChannels = new DVBTChannels();
	m_pMpeg2DataParser->SetDVBTChannels(m_pDVBTChannels);

	m_pPin = new CTSFileSourcePin(GetOwner(), this, phr);
	if (m_pPin == NULL)
	{
		if (phr)
			*phr = E_OUTOFMEMORY;
		return;
	}

	m_pTunerEvent = new TunerEvent(m_pDemux, GetOwner());
	m_pRegStore = new CRegStore("SOFTWARE\\TSFileSource");
	m_pSettingsStore = new CSettingsStore();

	// Load Registry Settings data
	GetRegStore("default");

	CMediaType cmt;
	cmt.InitMediaType();
	cmt.SetType(&MEDIATYPE_Stream);
	cmt.SetSubtype(&MEDIASUBTYPE_MPEG2_TRANSPORT);
	cmt.SetSubtype(&MEDIASUBTYPE_NULL);
	m_pPin->SetMediaType(&cmt);

	m_bThreadRunning = FALSE;
	m_bReload = FALSE;
	m_llLastMultiFileStart = 0;
	m_llLastMultiFileLength = 0;
	m_bColdStart = FALSE;

}

CTSFileSourceFilter::~CTSFileSourceFilter()
{
	//Make sure the worker thread is stopped before we exit.
	//Also closes the files.m_hThread
	if (CAMThread::ThreadExists())
	{
		CAMThread::CallWorker(CMD_STOP);
		CAMThread::CallWorker(CMD_EXIT);
		CAMThread::Close();
	}

	//Clear the filter list;
	POSITION pos = m_FilterRefList.GetHeadPosition();
	while (pos){

		if (m_FilterRefList.Get(pos) != NULL)
				m_FilterRefList.Get(pos)->Release();

		m_FilterRefList.Remove(pos);
		pos = m_FilterRefList.GetHeadPosition();
	}

	if (m_pMpeg2DataParser)
	{
		m_pMpeg2DataParser->ReleaseFilter();
		delete m_pMpeg2DataParser;
		m_pMpeg2DataParser = NULL;
	}

    if (m_dwGraphRegister)
    {
        RemoveGraphFromRot(m_dwGraphRegister);
        m_dwGraphRegister = 0;
    }

	m_pTunerEvent->UnRegisterForTunerEvents();
	m_pTunerEvent->Release();
	if (m_pDemux) delete	m_pDemux;
	if (m_pRegStore) delete m_pRegStore;
	if (m_pSettingsStore) delete  m_pSettingsStore;
	if (m_pPidParser) delete  m_pPidParser;
	if (m_pStreamParser) delete	m_pStreamParser;
	if (m_pPin) delete	m_pPin;
	if (m_pFileReader) delete	m_pFileReader;
	if (m_pFileDuration) delete  m_pFileDuration;
	if (m_pSampleBuffer) delete  m_pSampleBuffer;
	if (m_pSharedMemory) delete m_pSharedMemory;
	if (m_pClock) delete  m_pClock;
}

void CTSFileSourceFilter::UpdateThreadProc(void)
{
	m_WriteThreadActive = TRUE;
	REFERENCE_TIME rtLastCurrentTime = (REFERENCE_TIME)((REFERENCE_TIME)timeGetTime() * (REFERENCE_TIME)10000);

	int count = 1;

	while (!ThreadIsStopping(100))
	{
		HRESULT hr = S_OK;// if an error occurs.

		REFERENCE_TIME rtCurrentTime = (REFERENCE_TIME)((REFERENCE_TIME)timeGetTime() * (REFERENCE_TIME)10000);

		//Reparse the file for service change	
		if ((REFERENCE_TIME)(rtLastCurrentTime + (REFERENCE_TIME)RT_SECOND) < rtCurrentTime)
		{
			if(m_State != State_Stopped	&& TRUE)
			{
				//check pids every 5sec or quicker if no pids parsed
				if (count & 1 || !m_pPidParser->pidArray.Count())
				{
					UpdatePidParser(m_pFileReader);
				}

				//Change back to normal Auto operation if not already
				if (count == 6 && m_pPidParser->pidArray.Count() && m_bColdStart)
				{
					//Change back to normal Auto operation
					m_pDemux->set_Auto(m_bColdStart);
					m_bColdStart = FALSE; 
				}
			}

			count++;
			if (count > 6)
				count = 0;

			rtLastCurrentTime = (REFERENCE_TIME)((REFERENCE_TIME)timeGetTime() * (REFERENCE_TIME)10000);
		}

		Sleep(100);
	}
	m_WriteThreadActive = FALSE;
}

DWORD CTSFileSourceFilter::ThreadProc(void)
{
    HRESULT hr;  // the return code from calls
    Command com;

    do
    {
        com = GetRequest();
        if(com != CMD_INIT)
        {
			m_bThreadRunning = FALSE;
            DbgLog((LOG_ERROR, 1, TEXT("Thread expected init command")));
            Reply((DWORD) E_UNEXPECTED);
        }

    } while(com != CMD_INIT);

    DbgLog((LOG_TRACE, 1, TEXT("Worker thread initializing")));

	LPOLESTR fileName;
	m_pFileReader->GetFileName(&fileName);

	hr = m_pFileDuration->SetFileName(fileName);
	if (FAILED(hr))
    {
		m_bThreadRunning = FALSE;
		DbgLog((LOG_ERROR, 1, TEXT("ThreadCreate failed. Aborting thread.")));

        Reply(hr);  // send failed return code from ThreadCreate
        return 1;
    }

	hr = m_pFileDuration->OpenFile();
    if(FAILED(hr))
    {
		m_bThreadRunning = FALSE;
        DbgLog((LOG_ERROR, 1, TEXT("ThreadCreate failed. Aborting thread.")));

		hr = m_pFileDuration->CloseFile();
        Reply(hr);  // send failed return code from ThreadCreate
        return 1;
    }

	hr = m_pFileDuration->CloseFile();

    // Initialisation suceeded
    Reply(NOERROR);

    Command cmd;
    do
    {
        cmd = GetRequest();

        switch(cmd)
        {
            case CMD_EXIT:
				m_bThreadRunning = FALSE;
                Reply(NOERROR);
                break;

            case CMD_RUN:
                DbgLog((LOG_ERROR, 1, TEXT("CMD_RUN received before a CMD_PAUSE???")));
                // !!! fall through

            case CMD_PAUSE:
				if (SUCCEEDED(m_pFileDuration->OpenFile()))
				{
					m_bThreadRunning = TRUE;
					Reply(NOERROR);
					DoProcessingLoop();
					m_bThreadRunning = FALSE;
				}
                break;

            case CMD_STOP:
				m_pFileDuration->CloseFile();
				m_bThreadRunning = FALSE;
                Reply(NOERROR);
                break;

            default:
                DbgLog((LOG_ERROR, 1, TEXT("Unknown command %d received!"), cmd));
                Reply((DWORD) E_NOTIMPL);
                break;
        }

    } while(cmd != CMD_EXIT);

	m_pFileDuration->CloseFile();
	m_bThreadRunning = FALSE;
    DbgLog((LOG_TRACE, 1, TEXT("Worker thread exiting")));
    return 0;
}

//
// DoProcessingLoop
//
HRESULT CTSFileSourceFilter::DoProcessingLoop(void)
{
    Command com;

	m_pFileDuration->GetFileSize(&m_llLastMultiFileStart, &m_llLastMultiFileLength);
	REFERENCE_TIME rtLastCurrentTime = (REFERENCE_TIME)((REFERENCE_TIME)timeGetTime() * (REFERENCE_TIME)10000);

	int count = 1;

	BoostThread Boost;
    do
    {
        while(!CheckRequest(&com))
        {
			if (!m_WriteThreadActive)
				UpdateThread::StartThread();

			HRESULT hr = S_OK;// if an error occurs.

			REFERENCE_TIME rtCurrentTime = (REFERENCE_TIME)((REFERENCE_TIME)timeGetTime() * (REFERENCE_TIME)10000);

			WORD bReadOnly = FALSE;
			m_pFileDuration->get_ReadOnly(&bReadOnly);
			//Reparse the file for service change	
			if ((REFERENCE_TIME)(rtLastCurrentTime + (REFERENCE_TIME)RT_SECOND) < rtCurrentTime && bReadOnly)
			{
				CNetRender::UpdateNetFlow(&netArray);
				if(m_State != State_Stopped	&& TRUE)
				{
					__int64 fileStart, filelength;
					m_pFileDuration->GetFileSize(&fileStart, &filelength);

					//Get the FileReader Type
					WORD bMultiMode;
					m_pFileDuration->get_ReaderMode(&bMultiMode);
					//Do MultiFile timeshifting mode
					if((bMultiMode & ((__int64)(fileStart + (__int64)5000000) < m_llLastMultiFileStart))
						|| (bMultiMode & (fileStart == 0) & ((__int64)(filelength + (__int64)5000000) < m_llLastMultiFileLength))
						|| (!bMultiMode & ((__int64)(filelength + (__int64)5000000) < m_llLastMultiFileLength))
						&& TRUE)
					{
						LPOLESTR pszFileName;
						if (m_pFileDuration->GetFileName(&pszFileName) == S_OK)
						{
							LPOLESTR pFileName = new WCHAR[1+wcslen(pszFileName)];
							if (pFileName != NULL)
							{
								wcscpy(pFileName, pszFileName);
								load(pFileName, NULL);
								if (pFileName)
									delete[] pFileName;
							}
						}
					}
/*
					//check pids every 5sec or quicker if no pids parsed
					else if (count & 1 || !m_pPidParser->pidArray.Count())
//					else if (count == 5 || !m_pPidParser->pidArray.Count())
//					else if (!m_pPidParser->pidArray.Count())
					{
						if(m_pPidParser->get_ProgPinMode())
						{
							//update the parser
							UpdatePidParser(m_pFileReader);
						}
						else
						{
//							if (m_pDVBTChannels->GetListSize() > 0)
//							{
								UpdatePidParser(m_pFileReader);
//								m_pDVBTChannels->Clear();
//							}
						}

					}
*/
					m_llLastMultiFileStart = fileStart;
					m_llLastMultiFileLength = filelength;
				}
/*
				//Change back to normal Auto operation if not already
				if (count == 6 && m_pPidParser->pidArray.Count() && m_bColdStart)
				{
					//Change back to normal Auto operation
					m_pDemux->set_Auto(m_bColdStart);
					m_bColdStart = FALSE; 
				}

				count++;
				if (count > 6)
					count = 0;
*/
				rtLastCurrentTime = (REFERENCE_TIME)((REFERENCE_TIME)timeGetTime() * (REFERENCE_TIME)10000);
			}

			if (m_State != State_Stopped && bReadOnly)
			{
				if(!m_pPidParser->m_ParsingLock	&& m_pPidParser->pidArray.Count()
					&& TRUE) //cold start
				{
					hr = m_pPin->UpdateDuration(m_pFileDuration);
					if (hr == S_OK)
					{
						if (!m_bColdStart)
							if (m_pDemux->CheckDemuxPids() == S_FALSE)
							{
								m_pDemux->AOnConnect();
							}
					}
					else
						count = 0; //skip the pid update if seeking
				}
			}
/*
			if(m_State != State_Stopped && !m_pPidParser->get_ProgPinMode())
			{
				CComPtr<IBaseFilter>pMpegSections;
				if (SUCCEEDED(m_pDemux->GetParserFilter(pMpegSections)))
				{
					m_pMpeg2DataParser->SetFilter(pMpegSections);
					m_pMpeg2DataParser->SetDVBTChannels(m_pDVBTChannels);
					m_pMpeg2DataParser->StartScan();
				}
			}

*/			
			//randomly park the file pointer to help minimise HDD clogging
//			if (rtCurrentTime&1)
				m_pFileDuration->SetFilePointer(0, FILE_END);
//			else
//				m_pFileDuration->SetFilePointer(0, FILE_BEGIN);
			
			//kill the netrender graphs if were released

			if (netArray.Count() && CUnknown::m_cRef == 0)
				netArray.Clear();

			{
//				BrakeThread Brake;
				Sleep(100);
			}
        }

        // For all commands sent to us there must be a Reply call!
        if(com == CMD_RUN || com == CMD_PAUSE)
        {
			m_bThreadRunning = TRUE;
            Reply(NOERROR);
        }
        else if(com != CMD_STOP)
        {
            Reply((DWORD) E_UNEXPECTED);
            DbgLog((LOG_ERROR, 1, TEXT("Unexpected command!!!")));
        }
    } while(com != CMD_STOP);

	if (m_WriteThreadActive)
	{
		UpdateThread::StopThread(100);
		m_WriteThreadActive = FALSE;
	}
	m_pMpeg2DataParser->ReleaseFilter();
	m_bThreadRunning = FALSE;

    return S_FALSE;
}

BOOL CTSFileSourceFilter::ThreadRunning(void)
{ 
	return m_bThreadRunning;
}

STDMETHODIMP CTSFileSourceFilter::NonDelegatingQueryInterface(REFIID riid, void ** ppv)
{
	CheckPointer(ppv,E_POINTER);

	CAutoLock lock(&m_Lock);

	// Do we have this interface
	if (riid == IID_ITSFileSource)
	{
		return GetInterface((ITSFileSource*)this, ppv);
	}
	if (riid == IID_IFileSourceFilter)
	{
		return GetInterface((IFileSourceFilter*)this, ppv);
	}
	if (riid == IID_ISpecifyPropertyPages)
	{
		return GetInterface((ISpecifyPropertyPages*)this, ppv);
	}
	if (riid == IID_IMediaPosition || riid == IID_IMediaSeeking)
	{
		return m_pPin->NonDelegatingQueryInterface(riid, ppv);
	}
    if (riid == IID_IAMFilterMiscFlags)
    {
		return GetInterface((IAMFilterMiscFlags*)this, ppv);
    }
    if (riid == IID_IAMPushSource)
    {
		return GetInterface((IAMPushSource*)this, ppv);
    }
	if (riid == IID_IAMStreamSelect && (m_pDemux->get_Auto() | m_bColdStart))
	{
		return GetInterface((IAMStreamSelect*)this, ppv);
	}
	if (riid == IID_IReferenceClock)
	{
		return GetInterface((IReferenceClock*)m_pClock, ppv);
	}
	if (riid == IID_IAsyncReader)
		if ((!m_pPidParser->pids.pcr
			&& !get_AutoMode()
			&& m_pPidParser->get_ProgPinMode())
			&& m_pPidParser->get_AsyncMode())
		{
			return GetInterface((IAsyncReader*)this, ppv);
		}
	return CSource::NonDelegatingQueryInterface(riid, ppv);

} // NonDelegatingQueryInterface

//STDMETHODIMP_(ULONG) CTSFileSourceFilter::NonDelegatingRelease()
//{
//	if (CUnknown::m_cRef == 1)
//		netArray.Clear();

//	return CBaseFilter::NonDelegatingRelease();
//}

//IAMFilterMiscFlags
ULONG STDMETHODCALLTYPE CTSFileSourceFilter::GetMiscFlags(void)
{
	return (ULONG)AM_FILTER_MISC_FLAGS_IS_SOURCE; 
}//IAMFilterMiscFlags

//IAMPushSource
STDMETHODIMP CTSFileSourceFilter::GetPushSourceFlags(ULONG *pFlags)
{
	if (pFlags)
		return E_POINTER;

	*pFlags = AM_PUSHSOURCECAPS_NOT_LIVE;
	return S_OK;
}

STDMETHODIMP CTSFileSourceFilter::SetPushSourceFlags(ULONG Flags)
{
	return E_NOTIMPL;
}
        
STDMETHODIMP CTSFileSourceFilter::SetStreamOffset(REFERENCE_TIME rtOffset)
{
	return E_NOTIMPL;
}
        
STDMETHODIMP CTSFileSourceFilter::GetStreamOffset(REFERENCE_TIME *prtOffset)
{
	return E_NOTIMPL;
}
        
STDMETHODIMP CTSFileSourceFilter::GetMaxStreamOffset(REFERENCE_TIME *prtMaxOffset)
{
	return E_NOTIMPL;
}
        
STDMETHODIMP CTSFileSourceFilter::SetMaxStreamOffset(REFERENCE_TIME rtMaxOffset)
{
	return E_NOTIMPL;
}
        
STDMETHODIMP CTSFileSourceFilter::GetLatency(REFERENCE_TIME *prtLatency)
{
	return E_NOTIMPL;
}//IAMPushSource

STDMETHODIMP  CTSFileSourceFilter::Count(DWORD *pcStreams) //IAMStreamSelect
{
	if(!pcStreams)
		return E_INVALIDARG;

	CAutoLock SelectLock(&m_SelectLock);

	*pcStreams = 0;

	if (!m_pStreamParser->StreamArray.Count() ||
		!m_pPidParser->pidArray.Count() ||
		m_pPidParser->m_ParsingLock) //cold start
		return VFW_E_NOT_CONNECTED;

	*pcStreams = m_pStreamParser->StreamArray.Count();

	return S_OK;
} //IAMStreamSelect

STDMETHODIMP  CTSFileSourceFilter::Info( 
						long lIndex,
						AM_MEDIA_TYPE **ppmt,
						DWORD *pdwFlags,
						LCID *plcid,
						DWORD *pdwGroup,
						WCHAR **ppszName,
						IUnknown **ppObject,
						IUnknown **ppUnk) //IAMStreamSelect
{
	CAutoLock SelectLock(&m_SelectLock);

	//Check if file has been parsed
	if (!m_pPidParser->pidArray.Count() || m_pPidParser->m_ParsingLock)
		return E_FAIL;

	m_pStreamParser->ParsePidArray();
	m_pStreamParser->SetStreamActive(m_pPidParser->get_ProgramNumber());

	//Check if file has been parsed
	if (!m_pStreamParser->StreamArray.Count())
		return E_FAIL;
	
	//Check if in the bounds of index
	if(lIndex >= m_pStreamParser->StreamArray.Count() || lIndex < 0)
		return S_FALSE;

	if(ppmt) {

		AM_MEDIA_TYPE*	pmt = &m_pStreamParser->StreamArray[lIndex].media;
        *ppmt = (AM_MEDIA_TYPE *)CoTaskMemAlloc(sizeof(**ppmt));
        if (*ppmt == NULL)
            return E_OUTOFMEMORY;

		memcpy(*ppmt, pmt, sizeof(*pmt));

		if (pmt->cbFormat)
		{
			(*ppmt)->pbFormat = (BYTE*)CoTaskMemAlloc(pmt->cbFormat);
			memcpy((*ppmt)->pbFormat, pmt->pbFormat, pmt->cbFormat);
		}
	};

	if(pdwGroup)
		*pdwGroup = m_pStreamParser->StreamArray[lIndex].group;

	if(pdwFlags)
		*pdwFlags = m_pStreamParser->StreamArray[lIndex].flags;

	if(plcid)
		*plcid = m_pStreamParser->StreamArray[lIndex].lcid;

	if(ppszName) {

        *ppszName = (WCHAR *)CoTaskMemAlloc(sizeof(m_pStreamParser->StreamArray[lIndex].name));
        if (*ppszName == NULL)
            return E_OUTOFMEMORY;

		ZeroMemory(*ppszName, sizeof(m_pStreamParser->StreamArray[lIndex].name));
		wcscpy(*ppszName, m_pStreamParser->StreamArray[lIndex].name);
	}

	if(ppObject)
		*ppObject = (IUnknown *)m_pStreamParser->StreamArray[lIndex].object;

	if(ppUnk)
		*ppUnk = (IUnknown *)m_pStreamParser->StreamArray[lIndex].unk;

	return NOERROR;
} //IAMStreamSelect

STDMETHODIMP  CTSFileSourceFilter::Enable(long lIndex, DWORD dwFlags) //IAMStreamSelect
{
	CAutoLock SelectLock(&m_SelectLock);

	//Test if ready
	if (!m_pStreamParser->StreamArray.Count() ||
		!m_pPidParser->pidArray.Count() ||
		m_pPidParser->m_ParsingLock)
		return VFW_E_NOT_CONNECTED;

	//Test if out of bounds
	if (lIndex >= m_pStreamParser->StreamArray.Count() || lIndex < 0)
		return E_INVALIDARG;

	int indexOffset = netArray.Count() + (int)(netArray.Count() != 0);

	if (!lIndex)
		showEPGInfo();
	else if (lIndex && lIndex < m_pStreamParser->StreamArray.Count() - indexOffset - 2){

		m_pDemux->m_StreamVid = m_pStreamParser->StreamArray[lIndex].Vid;
		m_pDemux->m_StreamH264 = m_pStreamParser->StreamArray[lIndex].H264;
		m_pDemux->m_StreamMpeg4 = m_pStreamParser->StreamArray[lIndex].Mpeg4;
		m_pDemux->m_StreamAC3 = m_pStreamParser->StreamArray[lIndex].AC3;
		m_pDemux->m_StreamMP2 = m_pStreamParser->StreamArray[lIndex].Aud;
		m_pDemux->m_StreamAAC = m_pStreamParser->StreamArray[lIndex].AAC;
		m_pDemux->m_StreamDTS = m_pStreamParser->StreamArray[lIndex].DTS;
		m_pDemux->m_StreamAud2 = m_pStreamParser->StreamArray[lIndex].Aud2;
		set_PgmNumb((int)m_pStreamParser->StreamArray[lIndex].group + 1);
		BoostThread Boost;
		m_pStreamParser->SetStreamActive(m_pStreamParser->StreamArray[lIndex].group);
		m_pDemux->m_StreamVid = 0;
		m_pDemux->m_StreamH264 = 0;
		m_pDemux->m_StreamAC3 = 0;
		m_pDemux->m_StreamMP2 = 0;
		m_pDemux->m_StreamAud2 = 0;
		m_pDemux->m_StreamAAC = 0;
		m_pDemux->m_StreamDTS = 0;
		set_RegProgram();
	}
	else if (lIndex == m_pStreamParser->StreamArray.Count() - indexOffset - 2) //File Menu title
	{}
	else if (lIndex == m_pStreamParser->StreamArray.Count() - indexOffset - 1) //Load file Browser
	{
		load(L"", NULL);
	}
	else if (lIndex == m_pStreamParser->StreamArray.Count() - indexOffset) //Multicasting title
	{}
	else if (lIndex > m_pStreamParser->StreamArray.Count() - indexOffset) //Select multicast streams
	{
		WCHAR wfilename[MAX_PATH];
		wcscpy(wfilename, netArray[lIndex - (m_pStreamParser->StreamArray.Count() - netArray.Count())].fileName);
		if (SUCCEEDED(load(wfilename, NULL)))
		{
//			m_pFileReader->set_DelayMode(TRUE);
//			m_pFileDuration->set_DelayMode(TRUE);
			m_pFileReader->set_DelayMode(FALSE); //Cold Start
			m_pFileDuration->set_DelayMode(FALSE); //Cold Start
			REFERENCE_TIME stop, start = (__int64)max(0,(__int64)(m_pPidParser->pids.dur - RT_2_SECOND));
			IMediaSeeking *pMediaSeeking;
			if(GetFilterGraph() && SUCCEEDED(GetFilterGraph()->QueryInterface(IID_IMediaSeeking, (void **) &pMediaSeeking)))
			{
				pMediaSeeking->SetPositions(&start, AM_SEEKING_AbsolutePositioning , &stop, AM_SEEKING_AbsolutePositioning);
				pMediaSeeking->Release();
			}
		}
	}

	//Change back to normal Auto operation
	if (m_bColdStart)
	{
		//Change back to normal Auto operation
		m_pDemux->set_Auto(m_bColdStart);
		m_bColdStart = FALSE; //
	}

	return S_OK;

} //IAMStreamSelect


CBasePin * CTSFileSourceFilter::GetPin(int n)
{
	if (n == 0) {
		ASSERT(m_pPin);
		return m_pPin;
	} else {
		return NULL;
	}
}

int CTSFileSourceFilter::GetPinCount()
{
	return 1;
}

STDMETHODIMP CTSFileSourceFilter::FindPin(LPCWSTR Id, IPin ** ppPin)
{
    CheckPointer(ppPin,E_POINTER);
    ValidateReadWritePtr(ppPin,sizeof(IPin *));

	CAutoLock lock(&m_Lock);
	if (!wcscmp(Id, m_pPin->CBasePin::Name())) {

		*ppPin = m_pPin;
		if (*ppPin!=NULL){
			(*ppPin)->AddRef();
			return NOERROR;
		}
	}

	return CSource::FindPin(Id, ppPin);
}

void CTSFileSourceFilter::ResetStreamTime(void)
{
	CRefTime cTime;
	StreamTime(cTime);
	m_tStart = REFERENCE_TIME(m_tStart) + REFERENCE_TIME(cTime);
}

BOOL CTSFileSourceFilter::is_Active(void)
{
	return ((m_State == State_Paused) || (m_State == State_Running));
}

STDMETHODIMP CTSFileSourceFilter::Run(REFERENCE_TIME tStart)
{
	CAutoLock cObjectLock(m_pLock);

	if(!is_Active())
	{
		if (m_pFileReader->IsFileInvalid())
		{
			HRESULT hr = m_pFileReader->OpenFile();
			if (FAILED(hr))
				return hr;
		}

		if (m_pFileDuration->IsFileInvalid())
		{
			HRESULT hr = m_pFileDuration->OpenFile();
			if (FAILED(hr))
				return hr;
		}

		//Set our StreamTime Reference offset to zero
		m_tStart = tStart;

		REFERENCE_TIME start, stop;
		m_pPin->GetPositions(&start, &stop);

		//Start at least 100ms into file to skip header
		if (start == 0 && m_pPidParser->pidArray.Count())
			start += m_pPidParser->get_StartTimeOffset();

//***********************************************************************************************
//Old Capture format Additions
		if (m_pPidParser->pids.pcr){ 
//***********************************************************************************************
			m_pPin->m_DemuxLock = TRUE;
			m_pPin->setPositions(&start, AM_SEEKING_AbsolutePositioning , &stop, AM_SEEKING_NoPositioning);
//m_pPin->PrintTime(TEXT("Run"), (__int64) start, 10000);
			m_pPin->m_DemuxLock = FALSE;
//			m_pPin->m_IntBaseTimePCR = m_pPidParser->pids.start;
			m_pPin->m_IntStartTimePCR = m_pPidParser->pids.start;
			m_pPin->m_IntCurrentTimePCR = m_pPidParser->pids.start;
			m_pPin->m_IntEndTimePCR = m_pPidParser->pids.end;
		}

		set_TunerEvent();

		if (!m_bThreadRunning && CAMThread::ThreadExists())
			CAMThread::CallWorker(CMD_RUN);
	}

//	return CSource::Run(tStart);
	return CBaseFilter::Run(tStart);
}

HRESULT CTSFileSourceFilter::Pause()
{
//::OutputDebugString(TEXT("Pause In \n"));
	CAutoLock cObjectLock(m_pLock);

	if(!is_Active())
	{
		if (m_pFileReader->IsFileInvalid())
		{
			HRESULT hr = m_pFileReader->OpenFile();
			if (FAILED(hr))
				return hr;
		}

		if (m_pFileDuration->IsFileInvalid())
		{
			HRESULT hr = m_pFileDuration->OpenFile();
			if (FAILED(hr))
				return hr;
		}

		REFERENCE_TIME start, stop;
		m_pPin->GetPositions(&start, &stop);
//m_pPin->PrintTime(TEXT("Pause"), (__int64) start, 10000);

		//Start at least 100ms into file to skip header
		if (start == 0 && m_pPidParser->pidArray.Count())
			start += m_pPidParser->get_StartTimeOffset();

//***********************************************************************************************
//Old Capture format Additions
		if (m_pPidParser->pids.pcr){ 
//***********************************************************************************************
			m_pPin->m_DemuxLock = TRUE;
			m_pPin->setPositions(&start, AM_SEEKING_AbsolutePositioning , &stop, AM_SEEKING_NoPositioning);
			m_pPin->m_DemuxLock = FALSE;
//			m_pPin->m_IntBaseTimePCR = m_pPidParser->pids.start;
			m_pPin->m_IntStartTimePCR = m_pPidParser->pids.start;
			m_pPin->m_IntCurrentTimePCR = m_pPidParser->pids.start;
			m_pPin->m_IntEndTimePCR = m_pPidParser->pids.end;

		}
		
		//MSDemux fix
		if (start >= m_pPidParser->pids.dur)
		{
			IMediaSeeking *pMediaSeeking;
			if(GetFilterGraph() && SUCCEEDED(GetFilterGraph()->QueryInterface(IID_IMediaSeeking, (void **) &pMediaSeeking)))
			{
				REFERENCE_TIME stop, start = m_pPidParser->get_StartTimeOffset();
				HRESULT hr = pMediaSeeking->SetPositions(&start, AM_SEEKING_AbsolutePositioning , &stop, AM_SEEKING_NoPositioning);
				pMediaSeeking->Release();
			}
		}

		set_TunerEvent();

		if (!m_bThreadRunning && CAMThread::ThreadExists())
			CAMThread::CallWorker(CMD_PAUSE);

	}

	return CBaseFilter::Pause();
//	return CSource::Pause();
}

STDMETHODIMP CTSFileSourceFilter::Stop()
{
	CAutoLock lock(&m_Lock);
	CAutoLock cObjectLock(m_pLock);

//	if (m_bThreadRunning && CAMThread::ThreadExists())
//		CAMThread::CallWorker(CMD_STOP);
//	HRESULT hr = CSource::Stop();
	HRESULT hr = CBaseFilter::Stop();

	m_pTunerEvent->UnRegisterForTunerEvents();
	m_pFileReader->CloseFile();
	m_pFileDuration->CloseFile();

	return hr;
}

HRESULT CTSFileSourceFilter::FileSeek(REFERENCE_TIME seektime)
{
	if (m_pFileReader->IsFileInvalid())
	{
		return S_FALSE;
	}

	if (seektime > m_pPidParser->pids.dur)
	{
		return S_FALSE;
	}

	if (m_pPidParser->pids.dur > 10)
	{
		__int64 fileStart;
		__int64 filelength = 0;
		m_pFileReader->GetFileSize(&fileStart, &filelength);

		// shifting right by 14 rounds the seek and duration time down to the
		// nearest multiple 16.384 ms. More than accurate enough for our seeks.
		__int64 nFileIndex = 0;

		if (m_pPidParser->pids.dur>>14)
			nFileIndex = filelength * (__int64)(seektime>>14) / (__int64)(m_pPidParser->pids.dur>>14);

		nFileIndex = min(filelength, nFileIndex);
		nFileIndex = max(m_pPidParser->get_StartOffset(), nFileIndex);
		m_pFileReader->setFilePointer(nFileIndex, FILE_BEGIN);
	}
	return S_OK;
}

HRESULT CTSFileSourceFilter::OnConnect()
{
	BOOL wasThreadRunning = FALSE;
	if (m_bThreadRunning && CAMThread::ThreadExists()) {

		CAMThread::CallWorker(CMD_STOP);
		while (m_bThreadRunning){Sleep(10);};
		wasThreadRunning = TRUE;
	}

	HRESULT hr = m_pDemux->AOnConnect();

	m_pStreamParser->SetStreamActive(m_pPidParser->get_ProgramNumber());

	if (wasThreadRunning)
		CAMThread::CallWorker(CMD_RUN);

	return hr;
}

STDMETHODIMP CTSFileSourceFilter::GetPages(CAUUID *pPages)
{
	if (pPages == NULL) return E_POINTER;
	pPages->cElems = 1;
	pPages->pElems = (GUID*)CoTaskMemAlloc(sizeof(GUID));
	if (pPages->pElems == NULL)
	{
		return E_OUTOFMEMORY;
	}
	pPages->pElems[0] = CLSID_TSFileSourceProp;
	return S_OK;
}

STDMETHODIMP CTSFileSourceFilter::Load(LPCOLESTR pszFileName, const AM_MEDIA_TYPE *pmt)
{
	CAutoLock SelectLock(&m_SelectLock);
	return load(pszFileName, pmt);
}

HRESULT CTSFileSourceFilter::load(LPCOLESTR pszFileName, const AM_MEDIA_TYPE *pmt)
{
	// Is this a valid filename supplied
	CheckPointer(pszFileName,E_POINTER);

	LPOLESTR wFileName = new WCHAR[wcslen(pszFileName)+1];
	wcscpy(wFileName, pszFileName);

	if (_wcsicmp(wFileName, L"") == 0)
	{
		TCHAR tmpFile[MAX_PATH];
		LPTSTR ptFilename = (LPTSTR)&tmpFile;
		ptFilename[0] = '\0';

		// Setup the OPENFILENAME structure
		OPENFILENAME ofn = { sizeof(OPENFILENAME), NULL, NULL,
							 TEXT("Transport Stream Files (*.mpg, *.ts, *.tsbuffer, *.vob)\0*.mpg;*.ts;*.tsbuffer;*.vob\0All Files\0*.*\0\0"), NULL,
							 0, 1,
							 ptFilename, MAX_PATH,
							 NULL, 0,
							 NULL,
							 TEXT("Open Files (TS File Source Filter)"),
							 OFN_FILEMUSTEXIST|OFN_HIDEREADONLY, 0, 0,
							 NULL, 0, NULL, NULL };

		// Display the SaveFileName dialog.
		if( GetOpenFileName( &ofn ) != FALSE )
		{
			USES_CONVERSION;
			if(wFileName)
				delete[] wFileName;

			wFileName = new WCHAR[1+wcslen(T2W(ptFilename))];
			wcscpy(wFileName, T2W(ptFilename));
		}
		else
		{
			if(wFileName)
				delete[] wFileName;

			return NO_ERROR;
		}
	}

	HRESULT hr;

	//
	// Check & create a NetSource Filtergraph and play the file 
	//
	NetInfo *netAddr = new NetInfo();
	netAddr->rotEnable = m_bRotEnable;
	netAddr->bParserSink = m_bSharedMode;

	//
	// Check if the FileName is a Network address 
	//
	if (CNetRender::IsMulticastAddress(wFileName, netAddr))
	{
		//
		// Check in the local array if the Network address already active 
		//
		int pos = 0;
		if (!CNetRender::IsMulticastActive(netAddr, &netArray, &pos))
		{
//BoostThread Boost;
			//
			// Create the Network Filtergraph 
			//
			hr = CNetRender::CreateNetworkGraph(netAddr);
			if(FAILED(hr)  || (hr > 31))
			{
				delete netAddr;
				if(wFileName)
					delete[] wFileName;
//				MessageBoxW(NULL, netAddr->fileName, L"Graph Builder Failed", NULL);
				return hr;
			}
			//Add the new filtergraph settings to the local array
			netArray.Add(netAddr);
			if(wFileName)
				delete[] wFileName;

			wFileName = new WCHAR[1+wcslen(netAddr->fileName)];
			wcscpy(wFileName, netAddr->fileName);
//			m_pFileReader->set_DelayMode(TRUE);
//			m_pFileDuration->set_DelayMode(TRUE);
			m_pFileReader->set_DelayMode(FALSE);
			m_pFileDuration->set_DelayMode(FALSE);

		}
		else // If already running
		{
			if(wFileName)
				delete[] wFileName;

			wFileName = new WCHAR[1+wcslen(netArray[pos].fileName)];
			wcscpy(wFileName, netArray[pos].fileName);
			delete netAddr;
		}
	}
	else
		delete netAddr;

	for (int pos = 0; pos < netArray.Count(); pos++)
	{
		if (!wcsicmp(wFileName, netArray[pos].fileName))
			netArray[pos].playing = TRUE;
		else
			netArray[pos].playing = FALSE;
	}

	//Jump to a different Load method if already been set.
	if (m_bThreadRunning || is_Active() || m_pPin->CBasePin::IsConnected())
	{
		hr = ReLoad(wFileName, pmt);
		if(wFileName)
			delete[] wFileName;

		return hr;
	}

	BoostThread Boost;

	//Get delay Mode
	USHORT bDelay;
	m_pFileReader->get_DelayMode(&bDelay);

	//Get Pin Mode 
	BOOL pinModeSave = m_pPidParser->get_ProgPinMode();

	//Get ROT Mode 
	BOOL bRotEnable = m_bRotEnable;

	//Get Auto Mode 
	BOOL bAutoEnable = m_pDemux->get_Auto();

	//Get clock type
	int clock = m_pDemux->get_ClockMode();

	//Get Inject Mode 
	BOOL bInjectMode = m_pPin->get_InjectMode();

	//Get Rate Mode 
	BOOL bRateControl = m_pPin->get_RateControl();

	//Get NP Control Mode 
	BOOL bNPControl = m_pDemux->get_NPControl();

	//Get NP Slave Mode 
	BOOL bNPSlave = m_pDemux->get_NPSlave();

	//Get AC3 Mode 
	BOOL bAC3Mode = m_pDemux->get_AC3Mode();

	//Get Aspect Ratio Mode 
	BOOL bFixedAspectRatio = m_pDemux->get_FixedAspectRatio();

	//Get Create TS Pin Mode 
	BOOL bCreateTSPin = m_pDemux->get_CreateTSPinOnDemux();

	//Get Create Txt Pin Mode 
	BOOL bCreateTxtPin = m_pDemux->get_CreateTxtPinOnDemux();

	//Get Create Subtitle Pin Mode 
	BOOL bCreateSubPin = m_pDemux->get_CreateSubPinOnDemux();

	//Get MPEG2 Audio Media Type Mode 
	BOOL bMPEG2AudioMediaType = m_pDemux->get_MPEG2AudioMediaType();

	//Get Audio 2 Mode Mode 
	BOOL bAudio2Mode = m_pDemux->get_MPEG2Audio2Mode();

	delete m_pStreamParser;
	delete m_pDemux;
	delete m_pPidParser;
	delete m_pFileReader;
	delete m_pFileDuration;

	long length = wcslen(wFileName);
	if ((length < 9) || (_wcsicmp(wFileName+length-9, L".tsbuffer") != 0))
	{
		m_pFileReader = new FileReader(m_pSharedMemory);
		m_pFileDuration = new FileReader(m_pSharedMemory);//Get Live File Duration Thread
	}
	else
	{
		m_pFileReader = new MultiFileReader(m_pSharedMemory);
		m_pFileDuration = new MultiFileReader(m_pSharedMemory);
	}
	//m_pFileReader->SetDebugOutput(TRUE);
	//m_pFileDuration->SetDebugOutput(TRUE);

	m_pPidParser = new PidParser(m_pSampleBuffer, m_pFileReader);
	m_pDemux = new Demux(m_pPidParser, this, &m_FilterRefList);
	m_pStreamParser = new StreamParser(m_pPidParser, m_pDemux, &netArray);

	// Load Registry Settings data
	GetRegStore("default");
	
	//Check for forced pin mode
	if (pmt)
	{
		//Set Auto Mode if we had been told to previously
		m_pDemux->set_Auto(bAutoEnable); 

		//Set delay if we had been told to previously
		if (bDelay)
		{
			m_pFileReader->set_DelayMode(bDelay);
			m_pFileDuration->set_DelayMode(bDelay);
		}

		//Set ROT Mode if we had been told to previously
		m_bRotEnable = bRotEnable;

		//Set clock if we had been told to previously
		if (clock)
			m_pDemux->set_ClockMode(clock);

		m_pDemux->set_MPEG2AudioMediaType(bMPEG2AudioMediaType);
		m_pDemux->set_FixedAspectRatio(bFixedAspectRatio);
		m_pDemux->set_CreateTSPinOnDemux(bCreateTSPin);
		m_pDemux->set_CreateTxtPinOnDemux(bCreateTxtPin);
		m_pDemux->set_CreateSubPinOnDemux(bCreateSubPin);
		m_pDemux->set_AC3Mode(bAC3Mode);
		m_pDemux->set_NPSlave(bNPSlave);
		m_pDemux->set_NPControl(bNPControl);
		m_pDemux->set_MPEG2Audio2Mode(bAudio2Mode);
	}

	hr = m_pFileReader->SetFileName(wFileName);
	if (FAILED(hr))
	{
		if(wFileName)
			delete[] wFileName;

		return hr;
	}

	hr = m_pFileReader->OpenFile();
	if (FAILED(hr))
	{
		if (!m_pSharedMemory->GetShareMode())
			m_pSharedMemory->SetShareMode(TRUE);
		else
			m_pSharedMemory->SetShareMode(FALSE);

		hr = m_pFileReader->OpenFile();
		if (FAILED(hr))
		{
			if(wFileName)
				delete[] wFileName;

			return VFW_E_INVALIDMEDIATYPE;
		}
	}

	CAMThread::Create();			 //Create our GetDuration thread
	if (CAMThread::ThreadExists())
		CAMThread::CallWorker(CMD_INIT); //Initalize our GetDuration thread

	set_ROTMode();

	__int64 fileStart;
	__int64	fileSize = 0;
	m_pFileReader->GetFileSize(&fileStart, &fileSize);
	//If this a file start then return null.
	if (fileSize < MIN_FILE_SIZE)
	{
//		m_pFileReader->setFilePointer(0, FILE_BEGIN);
//		m_pPidParser->ParsePinMode();
		m_pPidParser->ParseFromFile(0);
		if (!m_pPidParser->pids.dur)
			m_pPidParser->pids.Clear();

		//Check for forced pin mode
		if (pmt)
		{
			//Set for cold start
			m_bColdStart = m_pDemux->get_Auto();
			m_pDemux->set_Auto(FALSE);
//			m_pClock->SetClockRate(0.99);

			if(MEDIATYPE_Stream == pmt->majortype)
			{
				//Are we in Transport mode
				if (MEDIASUBTYPE_MPEG2_TRANSPORT == pmt->subtype)
					m_pPidParser->set_ProgPinMode(FALSE);

				//Are we in Program mode
				else if (MEDIASUBTYPE_MPEG2_PROGRAM == pmt->subtype)
					m_pPidParser->set_ProgPinMode(TRUE);

				m_pPidParser->set_AsyncMode(FALSE);
			}
		}
		else
		{
//			m_pPidParser->ParsePinMode();
		}

		CMediaType cmt;
		cmt.InitMediaType();
		cmt.SetType(&MEDIATYPE_Stream);
		cmt.SetSubtype(&MEDIASUBTYPE_NULL);

		//Are we in Transport mode
		if (!m_pPidParser->get_ProgPinMode())
			cmt.SetSubtype(&MEDIASUBTYPE_MPEG2_TRANSPORT);
		//Are we in Program mode
		else 
			cmt.SetSubtype(&MEDIASUBTYPE_MPEG2_PROGRAM);

		m_pPin->SetMediaType(&cmt);

		{
			CAutoLock cObjectLock(m_pLock);
			m_pFileReader->CloseFile();
		}

		if(wFileName)
			delete[] wFileName;

		m_pPin->m_IntBaseTimePCR = 0;
		m_pPin->m_IntStartTimePCR = 0;
		m_pPin->m_IntCurrentTimePCR = 0;
		m_pPin->m_IntEndTimePCR = 0;
		m_pPin->SetDuration(0);

		IMediaSeeking *pMediaSeeking;
		if(GetFilterGraph() && SUCCEEDED(GetFilterGraph()->QueryInterface(IID_IMediaSeeking, (void **) &pMediaSeeking)))
		{
			CAutoLock cObjectLock(m_pLock);
			REFERENCE_TIME stop, start = 0;
			stop = 0;
			hr = pMediaSeeking->SetPositions(&start, AM_SEEKING_AbsolutePositioning , &stop, AM_SEEKING_AbsolutePositioning);
			pMediaSeeking->Release();
			NotifyEvent(EC_LENGTH_CHANGED, NULL, NULL);	
		}

		return S_OK;
	}

	m_pFileReader->setFilePointer(m_pPidParser->get_StartOffset(), FILE_BEGIN);

	RefreshPids();

	LoadPgmReg();
	RefreshDuration();

	//Check for forced pin mode
	if (pmt)
	{
		if(MEDIATYPE_Stream == pmt->majortype)
		{
			//Are we in Transport mode
			if (MEDIASUBTYPE_MPEG2_TRANSPORT == pmt->subtype)
				m_pPidParser->set_ProgPinMode(FALSE);

			//Are we in Program mode
			else if (MEDIASUBTYPE_MPEG2_PROGRAM == pmt->subtype)
				m_pPidParser->set_ProgPinMode(TRUE);

			m_pPidParser->set_AsyncMode(FALSE);
		}
	}

	CMediaType cmt;
	cmt.InitMediaType();
	cmt.SetType(&MEDIATYPE_Stream);
	cmt.SetSubtype(&MEDIASUBTYPE_NULL);

	//Are we in Transport mode
	if (!m_pPidParser->get_ProgPinMode())
		cmt.SetSubtype(&MEDIASUBTYPE_MPEG2_TRANSPORT);
	//Are we in Program mode
	else 
		cmt.SetSubtype(&MEDIASUBTYPE_MPEG2_PROGRAM);

	m_pPin->SetMediaType(&cmt);

	{
		CAutoLock cObjectLock(m_pLock);
		m_pFileReader->CloseFile();
	}
	
	if(wFileName)
		delete[] wFileName;

	m_pPin->m_IntBaseTimePCR = m_pPidParser->pids.start;
	m_pPin->m_IntStartTimePCR = m_pPidParser->pids.start;
	m_pPin->m_IntCurrentTimePCR = m_pPidParser->pids.start;
	m_pPin->m_IntEndTimePCR = m_pPidParser->pids.end;

	{
//		CAutoLock cObjectLock(m_pLock);
		IMediaSeeking *pMediaSeeking;
		if(GetFilterGraph() && SUCCEEDED(GetFilterGraph()->QueryInterface(IID_IMediaSeeking, (void **) &pMediaSeeking)))
		{
			CAutoLock cObjectLock(m_pLock);
			REFERENCE_TIME stop, start = m_pPidParser->get_StartTimeOffset();
			stop = m_pPidParser->pids.dur;
			hr = pMediaSeeking->SetPositions(&start, AM_SEEKING_AbsolutePositioning , &stop, AM_SEEKING_AbsolutePositioning);
			pMediaSeeking->Release();
			NotifyEvent(EC_LENGTH_CHANGED, NULL, NULL);	
		}
	}
	return S_OK;
}

STDMETHODIMP CTSFileSourceFilter::ReLoad(LPCOLESTR pszFileName, const AM_MEDIA_TYPE *pmt)
{
	HRESULT hr;

	BoostThread Boost;
	{
		//Test the file incase it doesn't exist,
		//also loads it into the File buffer for a smoother change over.
		FileReader *pFileReader = NULL;
		SharedMemory *pSharedMemory = NULL;
		long length = wcslen(pszFileName);
		if ((length < 9) || (_wcsicmp(pszFileName+length-9, L".tsbuffer") != 0))
		{
			pSharedMemory = new SharedMemory(64000000);
			pFileReader = new FileReader(pSharedMemory);
		}
		else
		{
			pSharedMemory = new SharedMemory(64000000);
			pFileReader = new MultiFileReader(pSharedMemory);
		}

		hr = pFileReader->SetFileName(pszFileName);
		if (FAILED(hr))
		{
			delete pFileReader;
			return hr;
		}

		hr = pFileReader->OpenFile();
		if (FAILED(hr))
		{
			if (!pSharedMemory->GetShareMode())
				pSharedMemory->SetShareMode(TRUE);
			else
				pSharedMemory->SetShareMode(FALSE);

			hr = pFileReader->OpenFile();
			if (FAILED(hr))
			{
				delete pFileReader;
				delete pSharedMemory;
				return VFW_E_INVALIDMEDIATYPE;
			}
		}

//		hr = pFileReader->OpenFile();
//		if (FAILED(hr))
//		{
//			delete pFileReader;
//			return VFW_E_INVALIDMEDIATYPE;
//		}

		pFileReader->CloseFile();
		delete pFileReader;
		delete pSharedMemory;
	}

	BOOL wasThreadRunning = FALSE;
	if (m_bThreadRunning && CAMThread::ThreadExists()) {

		CAMThread::CallWorker(CMD_STOP);
		while (m_bThreadRunning){Sleep(10);};
		wasThreadRunning = TRUE;
	}

	BOOL bState_Running = FALSE;
	BOOL bState_Paused = FALSE;

	if (m_State == State_Running)
		bState_Running = TRUE;
	else if (m_State == State_Paused)
		bState_Paused = TRUE;

	if (bState_Paused || bState_Running)
	{
		CAutoLock lock(&m_Lock);
		m_pDemux->DoStop();
	}

	//Get delay Mode
	USHORT bDelay;
	m_pFileReader->get_DelayMode(&bDelay);

	//Get Pin Mode 
	BOOL pinModeSave = m_pPidParser->get_ProgPinMode();

	//Get ROT Mode 
	BOOL bRotEnable = m_bRotEnable;

	//Get Auto Mode 
	BOOL bAutoEnable = m_pDemux->get_Auto();

	//Get clock type
	int clock = m_pDemux->get_ClockMode();

	//Get Inject Mode 
	BOOL bInjectMode = m_pPin->get_InjectMode();

	//Get Rate Mode 
	BOOL bRateControl = m_pPin->get_RateControl();

	//Get NP Control Mode 
	BOOL bNPControl = m_pDemux->get_NPControl();

	//Get NP Slave Mode 
	BOOL bNPSlave = m_pDemux->get_NPSlave();

	//Get AC3 Mode 
	BOOL bAC3Mode = m_pDemux->get_AC3Mode();

	//Get Aspect Ratio Mode 
	BOOL bFixedAspectRatio = m_pDemux->get_FixedAspectRatio();

	//Get Create TS Pin Mode 
	BOOL bCreateTSPin = m_pDemux->get_CreateTSPinOnDemux();

	//Get Create Txt Pin Mode 
	BOOL bCreateTxtPin = m_pDemux->get_CreateTxtPinOnDemux();

	//Get Create Subtitle Pin Mode 
	BOOL bCreateSubPin = m_pDemux->get_CreateSubPinOnDemux();

	//Get MPEG2 Audio Media Type Mode 
	BOOL bMPEG2AudioMediaType = m_pDemux->get_MPEG2AudioMediaType();

	//Get Audio 2 Mode Mode 
	BOOL bAudio2Mode = m_pDemux->get_MPEG2Audio2Mode();


	delete m_pStreamParser;
	delete m_pDemux;
	delete m_pPidParser;
	delete m_pFileReader;
	delete m_pFileDuration;

	long length = wcslen(pszFileName);
	if ((length < 9) || (_wcsicmp(pszFileName+length-9, L".tsbuffer") != 0))
	{
		m_pFileReader = new FileReader(m_pSharedMemory);
		m_pFileDuration = new FileReader(m_pSharedMemory);//Get Live File Duration Thread
	}
	else
	{
		m_pFileReader = new MultiFileReader(m_pSharedMemory);
		m_pFileDuration = new MultiFileReader(m_pSharedMemory);
	}
	//m_pFileReader->SetDebugOutput(TRUE);
	//m_pFileDuration->SetDebugOutput(TRUE);

	m_pPidParser = new PidParser(m_pSampleBuffer,m_pFileReader);
	m_pDemux = new Demux(m_pPidParser, this, &m_FilterRefList);
	m_pStreamParser = new StreamParser(m_pPidParser, m_pDemux, &netArray);

	//here we reset the shared memory buffers
	m_pSharedMemory->SetShareMode(FALSE);
	m_pSharedMemory->SetShareMode(TRUE);

	// Load Registry Settings data
	GetRegStore("default");

	//Check for forced pin mode
	if (TRUE)
	{
		//Set Auto Mode if we had been told to previously
		m_pDemux->set_Auto(bAutoEnable); 

		//Set delay if we had been told to previously
		if (bDelay)
		{
			m_pFileReader->set_DelayMode(bDelay);
			m_pFileDuration->set_DelayMode(bDelay);
		}

		//Get Rate Mode 
		m_pPin->set_RateControl(bRateControl);

		//Set ROT Mode if we had been told to previously
		m_bRotEnable = bRotEnable;

		//Set clock if we had been told to previously
		if (clock)
			m_pDemux->set_ClockMode(clock);

		m_pDemux->set_MPEG2AudioMediaType(bMPEG2AudioMediaType);
		m_pDemux->set_FixedAspectRatio(bFixedAspectRatio);
		m_pDemux->set_CreateTSPinOnDemux(bCreateTSPin);
		m_pDemux->set_CreateTxtPinOnDemux(bCreateTxtPin);
		m_pDemux->set_CreateSubPinOnDemux(bCreateSubPin);
		m_pDemux->set_AC3Mode(bAC3Mode);
		m_pDemux->set_NPSlave(bNPSlave);
		m_pDemux->set_NPControl(bNPControl);
		m_pDemux->set_MPEG2Audio2Mode(bAudio2Mode);
	}

	hr = m_pFileReader->SetFileName(pszFileName);
	if (FAILED(hr))
		return hr;

	hr = m_pFileReader->OpenFile();
	if (FAILED(hr))
	{
		if (!m_pSharedMemory->GetShareMode())
			m_pSharedMemory->SetShareMode(TRUE);
		else
			m_pSharedMemory->SetShareMode(FALSE);

		hr = m_pFileReader->OpenFile();
		if (FAILED(hr))
			return VFW_E_INVALIDMEDIATYPE;
	}

//	hr = m_pFileReader->OpenFile();
//	if (FAILED(hr))
//		return VFW_E_INVALIDMEDIATYPE;

	hr = m_pFileDuration->SetFileName(pszFileName);
	if (FAILED(hr))
		return hr;

	hr = m_pFileDuration->OpenFile();
	if (FAILED(hr))
		return VFW_E_INVALIDMEDIATYPE;

	set_ROTMode();

	__int64 fileStart;
	__int64	fileSize = 0;
	m_pFileReader->GetFileSize(&fileStart, &fileSize);
	m_llLastMultiFileStart = fileStart;
	m_llLastMultiFileLength = fileSize;

	int count = 0;
	__int64 fileSizeSave = fileSize;
/*	while(fileSize < 5000000 && count < 10)
	{
		count++;
		Sleep(500);
		m_pFileReader->GetFileSize(&fileStart, &fileSize);
		if (fileSize <= fileSizeSave)
		{
			NotifyEvent(EC_NEED_RESTART, NULL, NULL);
			Sleep(1000);
			break;
		}

		fileSizeSave = fileSize;
	};
*/
	while (fileSize < MIN_FILE_SIZE)
	{
//		m_pFileReader->setFilePointer(0, FILE_BEGIN);
//		m_pPidParser->ParsePinMode();
		m_pPidParser->ParseFromFile(0);
		if (m_pPidParser->pids.dur > 0)
		{
			m_pPidParser->pids.Clear();
			break;
		}
		else
			m_pPidParser->pids.Clear();

		//Check for forced pin mode
		if (pmt)
		{
			//Set for cold start
			m_bColdStart = m_pDemux->get_Auto();
			m_pDemux->set_Auto(FALSE);
//			m_pClock->SetClockRate(0.99);

			if(MEDIATYPE_Stream == pmt->majortype)
			{
				//Are we in Transport mode
				if (MEDIASUBTYPE_MPEG2_TRANSPORT == pmt->subtype)
					m_pPidParser->set_ProgPinMode(FALSE);

				//Are we in Program mode
				else if (MEDIASUBTYPE_MPEG2_PROGRAM == pmt->subtype)
					m_pPidParser->set_ProgPinMode(TRUE);

				m_pPidParser->set_AsyncMode(FALSE);
			}
		}
		else
		{
//			m_pPidParser->ParsePinMode();
		}

		CMediaType cmt;
		cmt.InitMediaType();
		cmt.SetType(&MEDIATYPE_Stream);
		cmt.SetSubtype(&MEDIASUBTYPE_NULL);

		//Are we in Transport mode
		if (!m_pPidParser->get_ProgPinMode())
			cmt.SetSubtype(&MEDIASUBTYPE_MPEG2_TRANSPORT);
		//Are we in Program mode
		else 
			cmt.SetSubtype(&MEDIASUBTYPE_MPEG2_PROGRAM);
		m_pPin->SetMediaType(&cmt);

		{
			CAutoLock cObjectLock(m_pLock);
			m_pFileReader->CloseFile();
		}

		IMediaSeeking *pMediaSeeking;
		if(GetFilterGraph() && SUCCEEDED(GetFilterGraph()->QueryInterface(IID_IMediaSeeking, (void **) &pMediaSeeking)))
		{
			CAutoLock cObjectLock(m_pLock);
			m_pPin->m_IntBaseTimePCR = 0;
			m_pPin->m_IntStartTimePCR = 0;
			m_pPin->m_IntCurrentTimePCR = 0;
			m_pPin->m_IntEndTimePCR = 0;
			m_pPin->SetDuration(0);
			REFERENCE_TIME stop, start = 0;
			stop = 0;
			hr = pMediaSeeking->SetPositions(&start, AM_SEEKING_AbsolutePositioning , &stop, AM_SEEKING_AbsolutePositioning);
			pMediaSeeking->Release();
			NotifyEvent(EC_LENGTH_CHANGED, NULL, NULL);	
		}
		return S_OK;
	};

	m_pFileReader->setFilePointer(m_pPidParser->get_StartOffset(), FILE_BEGIN);

	m_pPidParser->RefreshPids();
	LoadPgmReg();
	m_pStreamParser->ParsePidArray();
	RefreshDuration();
	m_pPin->m_IntBaseTimePCR = m_pPidParser->pids.start;
	m_pPin->m_IntStartTimePCR = m_pPidParser->pids.start;
	m_pPin->m_IntCurrentTimePCR = m_pPidParser->pids.start;
	m_pPin->m_IntEndTimePCR = m_pPidParser->pids.end;

	//Check for forced pin mode
	if (pmt)
	{
		if(MEDIATYPE_Stream == pmt->majortype)
		{
			//Are we in Transport mode
			if (MEDIASUBTYPE_MPEG2_TRANSPORT == pmt->subtype)
				m_pPidParser->set_ProgPinMode(FALSE);

			//Are we in Program mode
			else if (MEDIASUBTYPE_MPEG2_PROGRAM == pmt->subtype)
				m_pPidParser->set_ProgPinMode(TRUE);

			m_pPidParser->set_AsyncMode(FALSE);
		}
	}

	CMediaType cmt;
	cmt.InitMediaType();
	cmt.SetType(&MEDIATYPE_Stream);
	cmt.SetSubtype(&MEDIASUBTYPE_NULL);

	//Are we in Transport mode
	if (!m_pPidParser->get_ProgPinMode())
		cmt.SetSubtype(&MEDIASUBTYPE_MPEG2_TRANSPORT);
	//Are we in Program mode
	else 
		cmt.SetSubtype(&MEDIASUBTYPE_MPEG2_PROGRAM);

	m_pPin->SetMediaType(&cmt);

	// Reconnect Demux if pin mode has changed and Source is connected
	if (m_pPidParser->get_ProgPinMode() != pinModeSave && (IPin*)m_pPin->IsConnected())
	{
		m_pPin->ReNewDemux();
	}

	{
		CAutoLock cObjecLock(m_pLock);
		m_pDemux->AOnConnect();
	}
	m_pStreamParser->SetStreamActive(m_pPidParser->get_ProgramNumber());
//	m_rtLastCurrentTime = (REFERENCE_TIME)((REFERENCE_TIME)timeGetTime() * (REFERENCE_TIME)10000);


	if (bState_Paused || bState_Running)
	{
		CAutoLock cObjectLock(m_pLock);
		m_pDemux->DoStart();
	}
				
	if (bState_Paused)
	{
		CAutoLock cObjectLock(m_pLock);
		m_pDemux->DoPause();
	}

	{
//		CAutoLock cObjectLock(m_pLock);
		IMediaSeeking *pMediaSeeking;
		if(GetFilterGraph() && SUCCEEDED(GetFilterGraph()->QueryInterface(IID_IMediaSeeking, (void **) &pMediaSeeking)))
		{
			CAutoLock cObjectLock(m_pLock);
			REFERENCE_TIME stop, start = m_pPidParser->get_StartTimeOffset();
			stop = m_pPidParser->pids.dur;
			hr = pMediaSeeking->SetPositions(&start, AM_SEEKING_AbsolutePositioning , &stop, AM_SEEKING_AbsolutePositioning);
			pMediaSeeking->Release();
			NotifyEvent(EC_LENGTH_CHANGED, NULL, NULL);	
		}
	}

	if (wasThreadRunning)
		CAMThread::CallWorker(CMD_RUN);


	return S_OK;
}

HRESULT CTSFileSourceFilter::LoadPgmReg(void)
{

	HRESULT hr = S_OK;

	if (m_pPidParser->m_TStreamID && m_pPidParser->pidArray.Count() >= 2)
	{
		std::string saveName = m_pSettingsStore->getName();

		TCHAR cNID_TSID_ID[20];
		sprintf(cNID_TSID_ID, "%i:%i", m_pPidParser->m_NetworkID, m_pPidParser->m_TStreamID);

		// Load Registry Settings data
		GetRegStore(cNID_TSID_ID);

		if (m_pPidParser->set_ProgramSID() == S_OK)
		{
		}
		m_pSettingsStore->setName(saveName);
	}
	return hr;
}

HRESULT CTSFileSourceFilter::Refresh()
{
	CAutoLock lock(&m_Lock);

	 if (m_pFileReader)
		return UpdatePidParser(m_pFileReader);

	return E_FAIL;
}

HRESULT CTSFileSourceFilter::UpdatePidParser(FileReader *pFileReader)
{
	HRESULT hr = S_FALSE;// if an error occurs.

	REFERENCE_TIME start, stop;
	m_pPin->GetPositions(&start, &stop);

	PidParser *pPidParser = new PidParser(m_pSampleBuffer, pFileReader);
	
	int sid = m_pPidParser->pids.sid;
	int sidsave = m_pPidParser->m_ProgramSID;

//	BrakeThread Brake;
	if (pPidParser->RefreshPids() == S_OK)
	{
		int count = 0;
		while(m_pDemux->m_bConnectBusyFlag && count < 200)
		{
			{
	//			BrakeThread Brake;
				Sleep(10);
			}
	//		Sleep(100);
			count++;
		}
		m_pDemux->m_bConnectBusyFlag = TRUE;

	//	__int64 intBaseTimePCR = (__int64)min(m_pPidParser->pids.end, (__int64)(m_pPin->m_IntStartTimePCR - m_pPin->m_IntBaseTimePCR));
		__int64 intBaseTimePCR = parserFunctions.SubtractPCR(m_pPin->m_IntStartTimePCR, m_pPin->m_IntBaseTimePCR);
		intBaseTimePCR = (__int64)max(0, intBaseTimePCR);

		if (pPidParser->pidArray.Count())
		{
			hr = S_OK;

			//Check if we are locked out
			int count = 0;
			while (m_pPidParser->m_ParsingLock)
			{
				{
//					BrakeThread Brake;
					Sleep(10);
				}
//				Sleep(10);
				count++;
				if (count > 100)
				{
					delete  pPidParser;
					m_pDemux->m_bConnectBusyFlag = FALSE;
					return S_FALSE;
				}
			}
			//Lock the parser
			m_pPidParser->m_ParsingLock = TRUE;

			m_pPidParser->m_TStreamID = pPidParser->m_TStreamID;
			m_pPidParser->m_NetworkID = pPidParser->m_NetworkID;
			m_pPidParser->m_ONetworkID = pPidParser->m_ONetworkID;
//			m_pPidParser->m_ProgramSID = pPidParser->m_ProgramSID;
			m_pPidParser->m_ProgPinMode = pPidParser->m_ProgPinMode;
			m_pPidParser->m_AsyncMode = pPidParser->m_AsyncMode;
			m_pPidParser->m_PacketSize = pPidParser->m_PacketSize;
			m_pPidParser->m_ATSCFlag = pPidParser->m_ATSCFlag;
			memcpy(m_pPidParser->m_NetworkName, pPidParser->m_NetworkName, 128);
			memcpy(m_pPidParser->m_NetworkName + 127, "\0", 1);
			m_pPidParser->pids.CopyFrom(&pPidParser->pids);
			m_pPidParser->pidArray.Clear();

			m_pPin->WaitPinLock();
			for (int i = 0; i < pPidParser->pidArray.Count(); i++){
				PidInfo *pPids = new PidInfo;
				pPids->CopyFrom(&pPidParser->pidArray[i]);
				m_pPidParser->pidArray.Add(pPids);
			}
			//UnLock the parser
			m_pPidParser->m_ParsingLock	= FALSE;
		}

		if (m_pPidParser->m_TStreamID) 
		{
			if (sid)
				m_pPidParser->set_SIDPid(sid); //Setup for search
			else
				m_pPidParser->set_SIDPid(m_pPidParser->pids.sid); //Setup for search

			m_pPidParser->set_ProgramSID(); //set to same sid as before

			if (sidsave)
				m_pPidParser->m_ProgramSID = sidsave; // restore old sid reg setting.
			else
				m_pPidParser->m_ProgramSID = m_pPidParser->pids.sid; // restore old sid reg setting.
		}
						
		m_pStreamParser->ParsePidArray();
		m_pDemux->m_bConnectBusyFlag = FALSE;

		if (m_pDemux->CheckDemuxPids() == S_FALSE)
		{

//			m_pPin->m_IntBaseTimePCR = (__int64)min(m_pPidParser->pids.end, (__int64)(m_pPidParser->pids.start - intBaseTimePCR));
			m_pPin->m_IntBaseTimePCR = parserFunctions.SubtractPCR(m_pPidParser->pids.start, intBaseTimePCR);
			m_pPin->m_IntBaseTimePCR = (__int64)max(0, (__int64)(m_pPin->m_IntBaseTimePCR));
			m_pPin->m_IntStartTimePCR = m_pPidParser->pids.start;
			m_pPin->m_IntEndTimePCR = m_pPidParser->pids.end;

/*
	BOOL bState_Running = FALSE;
	BOOL bState_Paused = FALSE;

	if (m_State == State_Running)
		bState_Running = TRUE;
	else if (m_State == State_Paused)
		bState_Paused = TRUE;

	if (bState_Paused || bState_Running)
	{
//		CAutoLock lock(&m_Lock);
		m_pDemux->DoStop();
	}
*/
		m_pStreamParser->SetStreamActive(m_pPidParser->get_ProgramNumber());
			m_pDemux->AOnConnect();
//		set_PgmNumb(m_pPidParser->get_ProgramNumber());
//m_pPin->setPositions(&start, AM_SEEKING_AbsolutePositioning , &stop, AM_SEEKING_NoPositioning);
/*
	if (bState_Paused || bState_Running)
	{
////		CAutoLock cObjectLock(m_pLock);
		m_pDemux->DoStart();
	}
				
	if (bState_Paused)
	{
//		CAutoLock cObjectLock(m_pLock);
		m_pDemux->DoPause();
	}
*/
	{
//		CAutoLock cObjectLock(m_pLock);
		IMediaSeeking *pMediaSeeking;
		if(GetFilterGraph() && SUCCEEDED(GetFilterGraph()->QueryInterface(IID_IMediaSeeking, (void **) &pMediaSeeking)))
		{
			CAutoLock cObjectLock(m_pLock);
			start += RT_SECOND;
//			m_pPin->m_DemuxLock = TRUE;
			hr = pMediaSeeking->SetPositions(&start, AM_SEEKING_AbsolutePositioning , &stop, AM_SEEKING_NoPositioning);
//			m_pPin->m_DemuxLock = FALSE;
			pMediaSeeking->Release();
		}
	}
//TCHAR sz[128];
//sprintf(sz, "%u", 0);
//MessageBox(NULL, sz,"test", NULL);

		}
		else
			m_pStreamParser->SetStreamActive(m_pPidParser->get_ProgramNumber());

		m_pDemux->m_bConnectBusyFlag = FALSE;

	}
	delete  pPidParser;
	return hr;
}

HRESULT CTSFileSourceFilter::RefreshPids()
{
	HRESULT hr = m_pPidParser->ParseFromFile(m_pFileReader->getFilePointer());
	m_pStreamParser->ParsePidArray();
	return hr;
}

HRESULT CTSFileSourceFilter::RefreshDuration()
{
	return m_pPin->SetDuration(m_pPidParser->pids.dur);
}

STDMETHODIMP CTSFileSourceFilter::GetCurFile(LPOLESTR * ppszFileName,AM_MEDIA_TYPE *pmt)
{

	CheckPointer(ppszFileName, E_POINTER);
	*ppszFileName = NULL;

	LPOLESTR pFileName = NULL;
	HRESULT hr = m_pFileReader->GetFileName(&pFileName);
	if (FAILED(hr))
		return hr;

	if (pFileName != NULL)
	{
		*ppszFileName = (LPOLESTR)
		QzTaskMemAlloc(sizeof(WCHAR) * (1+wcslen(pFileName)));

		if (*ppszFileName != NULL)
		{
			wcscpy(*ppszFileName, pFileName);
		}
	}

	if(pmt)
	{
		ZeroMemory(pmt, sizeof(*pmt));
//		pmt->majortype = MEDIATYPE_Video;
		pmt->majortype = MEDIATYPE_Stream;

		//Are we in Program mode
		if (m_pPidParser && m_pPidParser->get_ProgPinMode())
			pmt->subtype = MEDIASUBTYPE_MPEG2_PROGRAM;
		else //Are we in Transport mode
			pmt->subtype = MEDIASUBTYPE_MPEG2_TRANSPORT;

		pmt->subtype = MEDIASUBTYPE_NULL;
	}

	return S_OK;
} 

STDMETHODIMP CTSFileSourceFilter::GetVideoPid(WORD *pVPid)
{
	if(!pVPid)
		return E_INVALIDARG;

	CAutoLock lock(&m_Lock);
	if (m_pPidParser->pids.vid)
		*pVPid = m_pPidParser->pids.vid;
	else if (m_pPidParser->pids.h264)
		*pVPid = m_pPidParser->pids.h264;
	else
		*pVPid = 0;

	return NOERROR;
}


STDMETHODIMP CTSFileSourceFilter::GetVideoPidType(BYTE *pointer)
{
	if (!pointer)
		  return E_INVALIDARG;

	CAutoLock lock(&m_Lock);
	if (m_pPidParser->pids.vid)
		sprintf((char *)pointer, "MPEG 2");
	else if (m_pPidParser->pids.h264)
		sprintf((char *)pointer, "H.264");
	else if (m_pPidParser->pids.mpeg4)
		sprintf((char *)pointer, "MPEG 4");
	else
		sprintf((char *)pointer, "None");

	return NOERROR;
}


STDMETHODIMP CTSFileSourceFilter::GetAudioPid(WORD *pAPid)
{
	if(!pAPid)
		return E_INVALIDARG;

	CAutoLock lock(&m_Lock);
	*pAPid = m_pPidParser->pids.aud;

	return NOERROR;

}

STDMETHODIMP CTSFileSourceFilter::GetAudio2Pid(WORD *pA2Pid)
{
	if(!pA2Pid)
		return E_INVALIDARG;

	CAutoLock lock(&m_Lock);
	*pA2Pid = m_pPidParser->pids.aud2;

	return NOERROR;

}

STDMETHODIMP CTSFileSourceFilter::GetAACPid(WORD *pAacPid)
{
	if(!pAacPid)
		return E_INVALIDARG;

	CAutoLock lock(&m_Lock);
	*pAacPid = m_pPidParser->pids.aac;

	return NOERROR;

}

STDMETHODIMP CTSFileSourceFilter::GetAAC2Pid(WORD *pAac2Pid)
{
	if(!pAac2Pid)
		return E_INVALIDARG;

	CAutoLock lock(&m_Lock);
	*pAac2Pid = m_pPidParser->pids.aac2;

	return NOERROR;
}

STDMETHODIMP CTSFileSourceFilter::GetDTSPid(WORD *pDtsPid)
{
	if(!pDtsPid)
		return E_INVALIDARG;

	CAutoLock lock(&m_Lock);
	*pDtsPid = m_pPidParser->pids.dts;

	return NOERROR;

}

STDMETHODIMP CTSFileSourceFilter::GetDTS2Pid(WORD *pDts2Pid)
{
	if(!pDts2Pid)
		return E_INVALIDARG;

	CAutoLock lock(&m_Lock);
	*pDts2Pid = m_pPidParser->pids.dts2;

	return NOERROR;

}
STDMETHODIMP CTSFileSourceFilter::GetAC3Pid(WORD *pAC3Pid)
{
	if(!pAC3Pid)
		return E_INVALIDARG;

	CAutoLock lock(&m_Lock);
	*pAC3Pid = m_pPidParser->pids.ac3;

	return NOERROR;

}

STDMETHODIMP CTSFileSourceFilter::GetAC3_2Pid(WORD *pAC3_2Pid)
{
	if(!pAC3_2Pid)
		return E_INVALIDARG;

	CAutoLock lock(&m_Lock);

	*pAC3_2Pid = m_pPidParser->pids.ac3_2;

	return NOERROR;

}

STDMETHODIMP CTSFileSourceFilter::GetTelexPid(WORD *pTelexPid)
{
	if(!pTelexPid)
		return E_INVALIDARG;

	CAutoLock lock(&m_Lock);

	*pTelexPid = m_pPidParser->pids.txt;

	return NOERROR;

}

STDMETHODIMP CTSFileSourceFilter::GetSubtitlePid(WORD *pSubPid)
{
	if(!pSubPid)
		return E_INVALIDARG;

	CAutoLock lock(&m_Lock);

	*pSubPid = m_pPidParser->pids.sub;

	return NOERROR;

}

STDMETHODIMP CTSFileSourceFilter::GetNIDPid(WORD *pNIDPid)
{
	if(!pNIDPid)
		return E_INVALIDARG;

	CAutoLock lock(&m_Lock);

	*pNIDPid = m_pPidParser->m_NetworkID;

	return NOERROR;

}

STDMETHODIMP CTSFileSourceFilter::GetONIDPid(WORD *pONIDPid)
{
	if(!pONIDPid)
		return E_INVALIDARG;

	CAutoLock lock(&m_Lock);
	*pONIDPid = m_pPidParser->m_ONetworkID;

	return NOERROR;

}

STDMETHODIMP CTSFileSourceFilter::GetTSIDPid(WORD *pTSIDPid)
{
	if(!pTSIDPid)
		return E_INVALIDARG;

	CAutoLock lock(&m_Lock);
	*pTSIDPid = m_pPidParser->m_TStreamID;

	return NOERROR;

}
	
STDMETHODIMP CTSFileSourceFilter::GetPMTPid(WORD *pPMTPid)
{
	if(!pPMTPid)
		return E_INVALIDARG;

	CAutoLock lock(&m_Lock);

	*pPMTPid = m_pPidParser->pids.pmt;

	return NOERROR;

}

STDMETHODIMP CTSFileSourceFilter::GetSIDPid(WORD *pSIDPid)
{
	if(!pSIDPid)
		return E_INVALIDARG;

	CAutoLock lock(&m_Lock);
	*pSIDPid = m_pPidParser->pids.sid;

	return NOERROR;

}

STDMETHODIMP CTSFileSourceFilter::GetPCRPid(WORD *pPCRPid)
{
	if(!pPCRPid)
		return E_INVALIDARG;

	CAutoLock lock(&m_Lock);
	*pPCRPid = m_pPidParser->pids.pcr - m_pPidParser->pids.opcr;

	return NOERROR;

}

STDMETHODIMP CTSFileSourceFilter::GetDuration(REFERENCE_TIME *dur)
{
	if(!dur)
		return E_INVALIDARG;

	CAutoLock lock(&m_Lock);
	*dur = m_pPidParser->pids.dur;

	return NOERROR;

}

STDMETHODIMP CTSFileSourceFilter::GetChannelNumber(BYTE *pointer)
{
	if (!pointer)
		  return E_INVALIDARG;

	CAutoLock lock(&m_Lock);
	m_pPidParser->get_ChannelNumber(pointer);

	return NOERROR;
}

STDMETHODIMP CTSFileSourceFilter::GetNetworkName(BYTE *pointer)
{
	if (!pointer)
		  return E_INVALIDARG;

	CAutoLock lock(&m_Lock);
	m_pPidParser->get_NetworkName(pointer);

	return NOERROR;
}

STDMETHODIMP CTSFileSourceFilter::GetONetworkName (BYTE *pointer)
{
	if (!pointer)
		  return E_INVALIDARG;

	CAutoLock lock(&m_Lock);
	m_pPidParser->get_ONetworkName(pointer);

	return NOERROR;
}

STDMETHODIMP CTSFileSourceFilter::GetChannelName(BYTE *pointer)
{
	if (!pointer)
		  return E_INVALIDARG;

	CAutoLock lock(&m_Lock);
	m_pPidParser->get_ChannelName(pointer);

	return NOERROR;
}

STDMETHODIMP CTSFileSourceFilter::GetEPGFromFile(void)
{
	CAutoLock lock(&m_Lock);
	return m_pPidParser->get_EPGFromFile();
}

STDMETHODIMP CTSFileSourceFilter::GetShortNextDescr (BYTE *pointer)
{
	if (!pointer)
		  return E_INVALIDARG;

	CAutoLock lock(&m_Lock);
	m_pPidParser->get_ShortNextDescr(pointer);

	return NOERROR;
}

STDMETHODIMP CTSFileSourceFilter::GetExtendedNextDescr (BYTE *pointer)
{
	if (!pointer)
		  return E_INVALIDARG;

	CAutoLock lock(&m_Lock);
	m_pPidParser->get_ExtendedNextDescr(pointer);

	return NOERROR;
}

STDMETHODIMP CTSFileSourceFilter::GetShortDescr (BYTE *pointer)
{
	if (!pointer)
		  return E_INVALIDARG;

	CAutoLock lock(&m_Lock);
	m_pPidParser->get_ShortDescr(pointer);

	return NOERROR;
}

STDMETHODIMP CTSFileSourceFilter::GetExtendedDescr (BYTE *pointer)
{
	if (!pointer)
		  return E_INVALIDARG;

	CAutoLock lock(&m_Lock);
	m_pPidParser->get_ExtendedDescr(pointer);

	return NOERROR;
}

STDMETHODIMP CTSFileSourceFilter::GetPgmNumb(WORD *pPgmNumb)
{
	if(!pPgmNumb)
		return E_INVALIDARG;

	CAutoLock lock(&m_Lock);
	*pPgmNumb = m_pPidParser->get_ProgramNumber() + 1;

	return NOERROR;

}

STDMETHODIMP CTSFileSourceFilter::GetPgmCount(WORD *pPgmCount)
{
	if(!pPgmCount)
		return E_INVALIDARG;

	CAutoLock lock(&m_Lock);
	*pPgmCount = m_pPidParser->pidArray.Count();

	return NOERROR;

}

STDMETHODIMP CTSFileSourceFilter::SetPgmNumb(WORD PgmNumb)
{
	CAutoLock lock(&m_Lock);
	return set_PgmNumb(PgmNumb);
}

HRESULT CTSFileSourceFilter::set_PgmNumb(WORD PgmNumb)
{
	//If only one program don't change it
	if (m_pPidParser->pidArray.Count() < 1)
		return NOERROR;

	REFERENCE_TIME start, stop;
	m_pPin->GetPositions(&start, &stop);

	int PgmNumber = PgmNumb;
	PgmNumber --;
	if (PgmNumber >= m_pPidParser->pidArray.Count())
	{
		PgmNumber = m_pPidParser->pidArray.Count() - 1;
	}
	else if (PgmNumber <= -1)
	{
		PgmNumber = 0;
	}
	
	BOOL wasThreadRunning = FALSE;
	if (m_bThreadRunning && CAMThread::ThreadExists()) {

		CAMThread::CallWorker(CMD_STOP);
		while (m_bThreadRunning){Sleep(10);};
		wasThreadRunning = TRUE;
	}

//	m_pPin->m_IntBaseTimePCR = (__int64)min(m_pPidParser->pids.end, (__int64)(m_pPin->m_IntStartTimePCR - m_pPin->m_IntBaseTimePCR));
	m_pPin->m_IntBaseTimePCR = parserFunctions.SubtractPCR(m_pPin->m_IntStartTimePCR, m_pPin->m_IntBaseTimePCR);
	m_pPin->m_IntBaseTimePCR = (__int64)max(0, (__int64)(m_pPin->m_IntBaseTimePCR));

//	m_pPin->m_DemuxLock = TRUE;
	m_pPidParser->set_ProgramNumber((WORD)PgmNumber);
	m_pPin->SetDuration(m_pPidParser->pids.dur);

//	m_pPin->m_IntBaseTimePCR = (__int64)min(m_pPidParser->pids.end, (__int64)(m_pPidParser->pids.start - m_pPin->m_IntBaseTimePCR));
	m_pPin->m_IntBaseTimePCR = parserFunctions.SubtractPCR(m_pPidParser->pids.start, m_pPin->m_IntBaseTimePCR);
	m_pPin->m_IntBaseTimePCR = (__int64)max(0, (__int64)(m_pPin->m_IntBaseTimePCR));

	m_pPin->m_IntStartTimePCR = m_pPidParser->pids.start;
	m_pPin->m_IntEndTimePCR = m_pPidParser->pids.end;
	OnConnect();

	ResetStreamTime();

	IMediaSeeking *pMediaSeeking;
	if(GetFilterGraph() && SUCCEEDED(GetFilterGraph()->QueryInterface(IID_IMediaSeeking, (void **) &pMediaSeeking)))
	{
		pMediaSeeking->SetPositions(&start, AM_SEEKING_AbsolutePositioning , &stop, AM_SEEKING_NoPositioning);
		pMediaSeeking->Release();
	}

//Sleep(5000);

	if (wasThreadRunning)
		CAMThread::CallWorker(CMD_RUN);

	return NOERROR;
}

STDMETHODIMP CTSFileSourceFilter::NextPgmNumb(void)
{
	CAutoLock lock(&m_Lock);

	//If only one program don't change it
	if (m_pPidParser->pidArray.Count() < 2)
		return NOERROR;

	REFERENCE_TIME start, stop;
	m_pPin->GetPositions(&start, &stop);

	WORD PgmNumb = m_pPidParser->get_ProgramNumber();
	PgmNumb++;
	if (PgmNumb >= m_pPidParser->pidArray.Count())
	{
		PgmNumb = 0;
	}

	BOOL wasThreadRunning = FALSE;
	if (m_bThreadRunning && CAMThread::ThreadExists()) {

		CAMThread::CallWorker(CMD_STOP);
		while (m_bThreadRunning){Sleep(10);};
		wasThreadRunning = TRUE;
	}

//	m_pPin->m_IntBaseTimePCR = (__int64)min(m_pPidParser->pids.end, (__int64)(m_pPin->m_IntStartTimePCR - m_pPin->m_IntBaseTimePCR));
	m_pPin->m_IntBaseTimePCR = parserFunctions.SubtractPCR(m_pPin->m_IntStartTimePCR, m_pPin->m_IntBaseTimePCR);
	m_pPin->m_IntBaseTimePCR = (__int64)max(0, (__int64)(m_pPin->m_IntBaseTimePCR));

//	m_pPin->m_DemuxLock = TRUE;
	m_pPidParser->set_ProgramNumber(PgmNumb);
	m_pPin->SetDuration(m_pPidParser->pids.dur);

//	m_pPin->m_IntBaseTimePCR = (__int64)min(m_pPidParser->pids.end, (__int64)(m_pPidParser->pids.start - m_pPin->m_IntBaseTimePCR));
	m_pPin->m_IntBaseTimePCR = parserFunctions.SubtractPCR(m_pPidParser->pids.start, m_pPin->m_IntBaseTimePCR);
	m_pPin->m_IntBaseTimePCR = (__int64)max(0, (__int64)(m_pPin->m_IntBaseTimePCR));

	m_pPin->m_IntStartTimePCR = m_pPidParser->pids.start;
	m_pPin->m_IntEndTimePCR = m_pPidParser->pids.end;
	OnConnect();
//	Sleep(200);
	ResetStreamTime();

	IMediaSeeking *pMediaSeeking;
	if(GetFilterGraph() && SUCCEEDED(GetFilterGraph()->QueryInterface(IID_IMediaSeeking, (void **) &pMediaSeeking)))
	{
		pMediaSeeking->SetPositions(&start, AM_SEEKING_AbsolutePositioning , &stop, AM_SEEKING_NoPositioning);
		pMediaSeeking->Release();
	}

//	m_pPin->setPositions(&start, AM_SEEKING_AbsolutePositioning, NULL, NULL);

//	m_pPin->m_DemuxLock = FALSE;

	if (wasThreadRunning)
		CAMThread::CallWorker(CMD_RUN);

	return NOERROR;
}

STDMETHODIMP CTSFileSourceFilter::PrevPgmNumb(void)
{
	CAutoLock lock(&m_Lock);

	//If only one program don't change it
	if (m_pPidParser->pidArray.Count() < 2)
		return NOERROR;

	REFERENCE_TIME start, stop;
	m_pPin->GetPositions(&start, &stop);

	int PgmNumb = m_pPidParser->get_ProgramNumber();
	PgmNumb--;
	if (PgmNumb < 0)
	{
		PgmNumb = m_pPidParser->pidArray.Count() - 1;
	}

	BOOL wasThreadRunning = FALSE;
	if (m_bThreadRunning && CAMThread::ThreadExists()) {

		CAMThread::CallWorker(CMD_STOP);
		while (m_bThreadRunning){Sleep(10);};
		wasThreadRunning = TRUE;
	}


//	m_pPin->m_IntBaseTimePCR = (__int64)min(m_pPidParser->pids.end, (__int64)(m_pPin->m_IntStartTimePCR - m_pPin->m_IntBaseTimePCR));
	m_pPin->m_IntBaseTimePCR = parserFunctions.SubtractPCR(m_pPin->m_IntStartTimePCR, m_pPin->m_IntBaseTimePCR);
	m_pPin->m_IntBaseTimePCR = (__int64)max(0, (__int64)(m_pPin->m_IntBaseTimePCR));

//	m_pPin->m_DemuxLock = TRUE;
	m_pPidParser->set_ProgramNumber((WORD)PgmNumb);
	m_pPin->SetDuration(m_pPidParser->pids.dur);

//	m_pPin->m_IntBaseTimePCR = (__int64)min(m_pPidParser->pids.end, (__int64)(m_pPidParser->pids.start - m_pPin->m_IntBaseTimePCR));
	m_pPin->m_IntBaseTimePCR = parserFunctions.SubtractPCR(m_pPidParser->pids.start, m_pPin->m_IntBaseTimePCR);
	m_pPin->m_IntBaseTimePCR = (__int64)max(0, (__int64)(m_pPin->m_IntBaseTimePCR));

	m_pPin->m_IntStartTimePCR = m_pPidParser->pids.start;
	m_pPin->m_IntEndTimePCR = m_pPidParser->pids.end;
	OnConnect();
//	Sleep(200);

	ResetStreamTime();

	IMediaSeeking *pMediaSeeking;
	if(GetFilterGraph() && SUCCEEDED(GetFilterGraph()->QueryInterface(IID_IMediaSeeking, (void **) &pMediaSeeking)))
	{
		pMediaSeeking->SetPositions(&start, AM_SEEKING_AbsolutePositioning , &stop, AM_SEEKING_NoPositioning);
		pMediaSeeking->Release();
	}

//	m_pPin->setPositions(&start, AM_SEEKING_AbsolutePositioning, NULL, NULL);

//	m_pPin->m_DemuxLock = FALSE;

	if (wasThreadRunning)
		CAMThread::CallWorker(CMD_RUN);

	return NOERROR;
}

HRESULT CTSFileSourceFilter::GetFileSize(__int64 *pStartPosition, __int64 *pEndPosition)
{
	CAutoLock lock(&m_Lock);
	return m_pFileReader->GetFileSize(pStartPosition, pEndPosition);
}

STDMETHODIMP CTSFileSourceFilter::GetTsArray(ULONG *pPidArray)
{
	if(!pPidArray)
		return E_INVALIDARG;

	CAutoLock lock(&m_Lock);

	m_pPidParser->get_CurrentTSArray(pPidArray);
	return NOERROR;
}

STDMETHODIMP CTSFileSourceFilter::GetAC3Mode(WORD *pAC3Mode)
{
	if(!pAC3Mode)
		return E_INVALIDARG;

	CAutoLock lock(&m_Lock);
	*pAC3Mode = m_pDemux->get_AC3Mode();
	return NOERROR;
}

STDMETHODIMP CTSFileSourceFilter::SetAC3Mode(WORD AC3Mode)
{
	CAutoLock lock(&m_Lock);
	m_pDemux->set_AC3Mode(AC3Mode);
	OnConnect();
	return NOERROR;
}

STDMETHODIMP CTSFileSourceFilter::GetMP2Mode(WORD *pMP2Mode)
{
	if(!pMP2Mode)
		return E_INVALIDARG;

	CAutoLock lock(&m_Lock);
	*pMP2Mode = m_pDemux->get_MPEG2AudioMediaType();
	return NOERROR;
}

STDMETHODIMP CTSFileSourceFilter::SetMP2Mode(WORD MP2Mode)
{
	CAutoLock lock(&m_Lock);
	m_pDemux->set_MPEG2AudioMediaType(MP2Mode);
	OnConnect();
	return NOERROR;
}

STDMETHODIMP CTSFileSourceFilter::GetAudio2Mode(WORD *pAudio2Mode)
{
	if(!pAudio2Mode)
		return E_INVALIDARG;

	CAutoLock lock(&m_Lock);
	*pAudio2Mode = m_pDemux->get_MPEG2Audio2Mode();
	return NOERROR;
}

STDMETHODIMP CTSFileSourceFilter::SetAudio2Mode(WORD Audio2Mode)
{
	CAutoLock lock(&m_Lock);
	m_pDemux->set_MPEG2Audio2Mode(Audio2Mode);
	OnConnect();
	return NOERROR;
}

STDMETHODIMP CTSFileSourceFilter::GetAutoMode(WORD *AutoMode)
{
	if(!AutoMode)
		return E_INVALIDARG;

	CAutoLock lock(&m_Lock);
	*AutoMode = m_pDemux->get_Auto();
	return NOERROR;
}

STDMETHODIMP CTSFileSourceFilter::SetAutoMode(WORD AutoMode)
{
	CAutoLock lock(&m_Lock);
	m_pDemux->set_Auto(AutoMode);
	OnConnect();
	return NOERROR;
}

STDMETHODIMP CTSFileSourceFilter::GetNPControl(WORD *NPControl)
{
	if(!NPControl)
		return E_INVALIDARG;

	CAutoLock lock(&m_Lock);
	*NPControl = m_pDemux->get_NPControl();
	return NOERROR;
}

STDMETHODIMP CTSFileSourceFilter::SetNPControl(WORD NPControl)
{
	CAutoLock lock(&m_Lock);
	m_pDemux->set_NPControl(NPControl);
	OnConnect();
	return NOERROR;
}

STDMETHODIMP CTSFileSourceFilter::GetNPSlave(WORD *NPSlave)
{
	if(!NPSlave)
		return E_INVALIDARG;

	CAutoLock lock(&m_Lock);
	*NPSlave = m_pDemux->get_NPSlave();
	return NOERROR;
}

STDMETHODIMP CTSFileSourceFilter::SetNPSlave(WORD NPSlave)
{
	CAutoLock lock(&m_Lock);
	m_pDemux->set_NPSlave(NPSlave);
	OnConnect();
	return NOERROR;
}

HRESULT CTSFileSourceFilter::set_TunerEvent(void)
{
	if (GetFilterGraph() && SUCCEEDED(m_pTunerEvent->HookupGraphEventService(GetFilterGraph())))
	{
		m_pTunerEvent->RegisterForTunerEvents();
	}
	return NOERROR;
}

STDMETHODIMP CTSFileSourceFilter::SetTunerEvent(void)
{
	CAutoLock lock(&m_Lock);
	return set_TunerEvent();
}

STDMETHODIMP CTSFileSourceFilter::GetDelayMode(WORD *DelayMode)
{
	if(!DelayMode)
		return E_INVALIDARG;

	CAutoLock lock(&m_Lock);
	m_pFileReader->get_DelayMode(DelayMode);
	return NOERROR;
}

STDMETHODIMP CTSFileSourceFilter::SetDelayMode(WORD DelayMode)
{
	CAutoLock lock(&m_Lock);
	m_pFileReader->set_DelayMode(DelayMode);
	OnConnect();
	return NOERROR;
}

STDMETHODIMP CTSFileSourceFilter::GetSharedMode(WORD* pSharedMode)
{
	if (!pSharedMode)
		return E_INVALIDARG;

	CAutoLock lock(&m_Lock);
	*pSharedMode = m_bSharedMode;
	return NOERROR;
}

STDMETHODIMP CTSFileSourceFilter::SetSharedMode(WORD SharedMode)
{
	CAutoLock lock(&m_Lock);
	m_bSharedMode = SharedMode;
	return NOERROR;
}

STDMETHODIMP CTSFileSourceFilter::GetInjectMode(WORD* pInjectMode)
{
	if (!pInjectMode)
		return E_INVALIDARG;

	CAutoLock lock(&m_Lock);
	*pInjectMode = m_pPin->get_InjectMode();
	return NOERROR;
}

STDMETHODIMP CTSFileSourceFilter::SetInjectMode(WORD InjectMode)
{
	CAutoLock lock(&m_Lock);
	m_pPin->set_InjectMode(InjectMode);
	return NOERROR;
}

STDMETHODIMP CTSFileSourceFilter::GetRateControlMode(WORD* pRateControl)
{
	if (!pRateControl)
		return E_INVALIDARG;

	CAutoLock lock(&m_Lock);
	*pRateControl = m_pPin->get_RateControl();
	return NOERROR;
}

STDMETHODIMP CTSFileSourceFilter::SetRateControlMode(WORD RateControl)
{
	CAutoLock lock(&m_Lock);
	m_pPin->set_RateControl(RateControl);
	return NOERROR;
}

STDMETHODIMP CTSFileSourceFilter::GetFixedAspectRatio(WORD *pbFixedAR)
{
	if(!pbFixedAR)
		return E_INVALIDARG;

	CAutoLock lock(&m_Lock);
	*pbFixedAR = m_pDemux->get_FixedAspectRatio();
	return NOERROR;
}

STDMETHODIMP CTSFileSourceFilter::SetFixedAspectRatio(WORD bFixedAR)
{
	CAutoLock lock(&m_Lock);
	m_pDemux->set_FixedAspectRatio(bFixedAR);
	return NOERROR;
}

STDMETHODIMP CTSFileSourceFilter::GetCreateTSPinOnDemux(WORD *pbCreatePin)
{
	if(!pbCreatePin)
		return E_INVALIDARG;

	CAutoLock lock(&m_Lock);
	*pbCreatePin = m_pDemux->get_CreateTSPinOnDemux();
	return NOERROR;
}

STDMETHODIMP CTSFileSourceFilter::SetCreateTSPinOnDemux(WORD bCreatePin)
{
	CAutoLock lock(&m_Lock);
	m_pDemux->set_CreateTSPinOnDemux(bCreatePin);
	OnConnect();
	return NOERROR;
}

STDMETHODIMP CTSFileSourceFilter::GetCreateTxtPinOnDemux(WORD *pbCreatePin)
{
	if(!pbCreatePin)
		return E_INVALIDARG;

	CAutoLock lock(&m_Lock);
	*pbCreatePin = m_pDemux->get_CreateTxtPinOnDemux();
	return NOERROR;
}

STDMETHODIMP CTSFileSourceFilter::SetCreateTxtPinOnDemux(WORD bCreatePin)
{
	CAutoLock lock(&m_Lock);
	m_pDemux->set_CreateTxtPinOnDemux(bCreatePin);
	OnConnect();
	return NOERROR;
}

STDMETHODIMP CTSFileSourceFilter::GetCreateSubPinOnDemux(WORD *pbCreatePin)
{
	if(!pbCreatePin)
		return E_INVALIDARG;

	CAutoLock lock(&m_Lock);
	*pbCreatePin = m_pDemux->get_CreateSubPinOnDemux();
	return NOERROR;
}

STDMETHODIMP CTSFileSourceFilter::SetCreateSubPinOnDemux(WORD bCreatePin)
{
	CAutoLock lock(&m_Lock);
	m_pDemux->set_CreateSubPinOnDemux(bCreatePin);
	OnConnect();
	return NOERROR;
}

STDMETHODIMP CTSFileSourceFilter::GetReadOnly(WORD *ReadOnly)
{
	if(!ReadOnly)
		return E_INVALIDARG;

	CAutoLock lock(&m_Lock);
	m_pFileReader->get_ReadOnly(ReadOnly);
	return NOERROR;
}

STDMETHODIMP CTSFileSourceFilter::GetBitRate(long *pRate)
{
    if(!pRate)
        return E_INVALIDARG;

    CAutoLock lock(&m_Lock);
    *pRate = m_pPin->get_BitRate();

    return NOERROR;
}

STDMETHODIMP CTSFileSourceFilter::SetBitRate(long Rate)
{
    CAutoLock lock(&m_Lock);
    m_pPin->set_BitRate(Rate);

    return NOERROR;

}

STDMETHODIMP CTSFileSourceFilter::GetROTMode(WORD *ROTMode)
{
	if(!ROTMode)
		return E_INVALIDARG;

	CAutoLock lock(&m_Lock);
	*ROTMode = m_bRotEnable;
	return NOERROR;
}

STDMETHODIMP CTSFileSourceFilter::SetROTMode(WORD ROTMode)
{
	CAutoLock lock(&m_Lock);
	m_bRotEnable = ROTMode;
	set_ROTMode();
	return NOERROR;
}

STDMETHODIMP CTSFileSourceFilter::GetClockMode(WORD *ClockMode)
{
	if(!ClockMode)
		return E_INVALIDARG;

	CAutoLock lock(&m_Lock);
	*ClockMode = m_pDemux->get_ClockMode();
	return NOERROR;
}

STDMETHODIMP CTSFileSourceFilter::SetClockMode(WORD ClockMode)
{
	CAutoLock lock(&m_Lock);
	m_pDemux->set_ClockMode(ClockMode);
	m_pDemux->SetRefClock();
	return NOERROR;
}

STDMETHODIMP CTSFileSourceFilter::SetRegStore(LPTSTR nameReg)
{

	char name[128] = "";
	sprintf(name, "%s", nameReg);

	if ((strcmp(name, "user")!=0) && (strcmp(name, "default")!=0))
	{
		std::string saveName = m_pSettingsStore->getName();
		m_pSettingsStore->setName(nameReg);
		m_pSettingsStore->setProgramSIDReg((int)m_pPidParser->pids.sid);
		m_pSettingsStore->setAudio2ModeReg((BOOL)m_pDemux->get_MPEG2Audio2Mode());
		m_pSettingsStore->setAC3ModeReg((BOOL)m_pDemux->get_AC3Mode());
		m_pRegStore->setSettingsInfo(m_pSettingsStore);
		m_pSettingsStore->setName(saveName);
	}
	else
	{
		WORD delay;
		m_pFileReader->get_DelayMode(&delay);
		m_pSettingsStore->setDelayModeReg((BOOL)delay);
		m_pSettingsStore->setSharedModeReg((BOOL)m_bSharedMode);
		m_pSettingsStore->setInjectModeReg((BOOL)m_pPin->get_InjectMode());
		m_pSettingsStore->setRateControlModeReg((BOOL)m_pPin->get_RateControl());
		m_pSettingsStore->setAutoModeReg((BOOL)m_pDemux->get_Auto());
		m_pSettingsStore->setNPControlReg((BOOL)m_pDemux->get_NPControl());
		m_pSettingsStore->setNPSlaveReg((BOOL)m_pDemux->get_NPSlave());
		m_pSettingsStore->setMP2ModeReg((BOOL)m_pDemux->get_MPEG2AudioMediaType());
		m_pSettingsStore->setAudio2ModeReg((BOOL)m_pDemux->get_MPEG2Audio2Mode());
		m_pSettingsStore->setAC3ModeReg((BOOL)m_pDemux->get_AC3Mode());
		m_pSettingsStore->setFixedAspectRatioReg((BOOL)m_pDemux->get_FixedAspectRatio());
		m_pSettingsStore->setCreateTSPinOnDemuxReg((BOOL)m_pDemux->get_CreateTSPinOnDemux());
		m_pSettingsStore->setROTModeReg((int)m_bRotEnable);
		m_pSettingsStore->setClockModeReg((BOOL)m_pDemux->get_ClockMode());
		m_pSettingsStore->setCreateTxtPinOnDemuxReg((BOOL)m_pDemux->get_CreateTxtPinOnDemux());

		m_pRegStore->setSettingsInfo(m_pSettingsStore);
	}
    return NOERROR;
}

STDMETHODIMP CTSFileSourceFilter::GetRegStore(LPTSTR nameReg)
{
	char name[128] = "";
	sprintf(name, "%s", nameReg);

	std::string saveName = m_pSettingsStore->getName();

	// Load Registry Settings data
	m_pSettingsStore->setName(nameReg);

	if(m_pRegStore->getSettingsInfo(m_pSettingsStore))
	{
		if ((strcmp(name, "user")!=0) && (strcmp(name, "default")!=0))
		{
			m_pPidParser->set_SIDPid(m_pSettingsStore->getProgramSIDReg());
			m_pDemux->set_AC3Mode(m_pSettingsStore->getAC3ModeReg());
			m_pDemux->set_MPEG2Audio2Mode(m_pSettingsStore->getAudio2ModeReg());
			m_pSettingsStore->setName(saveName);
		}
		else
		{	
			m_pFileReader->set_DelayMode(m_pSettingsStore->getDelayModeReg());
			m_pFileDuration->set_DelayMode(m_pSettingsStore->getDelayModeReg());
			m_pDemux->set_Auto(m_pSettingsStore->getAutoModeReg());
			m_pDemux->set_NPControl(m_pSettingsStore->getNPControlReg());
			m_pDemux->set_NPSlave(m_pSettingsStore->getNPSlaveReg());
			m_pDemux->set_MPEG2AudioMediaType(m_pSettingsStore->getMP2ModeReg());
			m_pDemux->set_MPEG2Audio2Mode(m_pSettingsStore->getAudio2ModeReg());
			m_pDemux->set_AC3Mode(m_pSettingsStore->getAC3ModeReg());
			m_pDemux->set_FixedAspectRatio(m_pSettingsStore->getFixedAspectRatioReg());
			m_pDemux->set_CreateTSPinOnDemux(m_pSettingsStore->getCreateTSPinOnDemuxReg());
			m_bSharedMode = m_pSettingsStore->getSharedModeReg();
			m_pPin->set_InjectMode(m_pSettingsStore->getInjectModeReg());
			m_pPin->set_RateControl(m_pSettingsStore->getRateControlModeReg());
			m_bRotEnable = m_pSettingsStore->getROTModeReg();
			m_pDemux->set_ClockMode(m_pSettingsStore->getClockModeReg());
			m_pDemux->set_CreateTxtPinOnDemux(m_pSettingsStore->getCreateTxtPinOnDemuxReg());
		}
	}

    return NOERROR;
}

STDMETHODIMP CTSFileSourceFilter::SetRegSettings()
{
	CAutoLock lock(&m_Lock);
	SetRegStore("user");
    return NOERROR;
}


STDMETHODIMP CTSFileSourceFilter::GetRegSettings()
{
	CAutoLock lock(&m_Lock);
	GetRegStore("user");
    return NOERROR;
}

HRESULT CTSFileSourceFilter::set_RegProgram()
{
	if (m_pPidParser->pids.sid && m_pPidParser->m_TStreamID)
	{
		TCHAR cNID_TSID_ID[32];
		sprintf(cNID_TSID_ID, "%i:%i", m_pPidParser->m_NetworkID, m_pPidParser->m_TStreamID);
		SetRegStore(cNID_TSID_ID);
	}
    return NOERROR;
}

STDMETHODIMP CTSFileSourceFilter::SetRegProgram()
{
	CAutoLock lock(&m_Lock);
	return set_RegProgram();
}

STDMETHODIMP CTSFileSourceFilter::ShowFilterProperties()
{
	CAutoLock cObjectLock(m_pLock);

//    HWND    phWnd = (HWND)CreateEvent(NULL, FALSE, FALSE, NULL);

	ULONG refCount;
	IEnumFilters * piEnumFilters = NULL;
	if (GetFilterGraph() && SUCCEEDED(GetFilterGraph()->EnumFilters(&piEnumFilters)))
	{
		IBaseFilter * pFilter;
		while (piEnumFilters->Next(1, &pFilter, 0) == NOERROR )
		{
			ISpecifyPropertyPages* piProp = NULL;
			if ((pFilter->QueryInterface(IID_ISpecifyPropertyPages, (void **)&piProp) == S_OK) && (piProp != NULL))
			{
				FILTER_INFO filterInfo;
				if (pFilter->QueryFilterInfo(&filterInfo) == S_OK)
				{
					LPOLESTR fileName = NULL;
					m_pFileReader->GetFileName(&fileName);
			
					if (fileName && !wcsicmp(fileName, filterInfo.achName))
					{
						CAUUID caGUID;
						piProp->GetPages(&caGUID);
						if(caGUID.cElems)
						{
							IUnknown *piFilterUnk = NULL;
							if (pFilter->QueryInterface(IID_IUnknown, (void **)&piFilterUnk) == S_OK)
							{
								OleCreatePropertyFrame(0, 0, 0, filterInfo.achName, 1, &piFilterUnk, caGUID.cElems, caGUID.pElems, 0, 0, NULL);
								piFilterUnk->Release();
							}
							CoTaskMemFree(caGUID.pElems);
						}
					}
					filterInfo.pGraph->Release(); 
				}
				piProp->Release();
			}
			refCount = pFilter->Release();
			pFilter = NULL;
		}
		refCount = piEnumFilters->Release();
	}
//	CloseHandle(phWnd);
	return NOERROR;
}

BOOL CTSFileSourceFilter::get_AutoMode()
{
	return m_pDemux->get_Auto();
}

BOOL CTSFileSourceFilter::get_PinMode()
{
	return m_pPidParser->get_ProgPinMode();;
}

// Adds a DirectShow filter graph to the Running Object Table,
// allowing GraphEdit to "spy" on a remote filter graph.
HRESULT CTSFileSourceFilter::AddGraphToRot(
        IUnknown *pUnkGraph, 
        DWORD *pdwRegister
        ) 
{
    CComPtr <IMoniker>              pMoniker;
    CComPtr <IRunningObjectTable>   pROT;
    WCHAR wsz[128];
    HRESULT hr;

    if (FAILED(GetRunningObjectTable(0, &pROT)))
        return E_FAIL;

    wsprintfW(wsz, L"FilterGraph %08x pid %08x\0", (DWORD_PTR) pUnkGraph, 
              GetCurrentProcessId());
/*	
	//Search the ROT for the same reference
	IUnknown *pUnk = NULL;
	if (SUCCEEDED(GetObjectFromROT(wsz, &pUnk)))
	{
		//Exit out if we have an object running in ROT
		if (pUnk)
		{
			pUnk->Release();
			return S_OK;
		}
	}
*/
    hr = CreateItemMoniker(L"!", wsz, &pMoniker);
    if (SUCCEEDED(hr))
	{
        hr = pROT->Register(ROTFLAGS_REGISTRATIONKEEPSALIVE, pUnkGraph, 
                            pMoniker, pdwRegister);
	}
    return hr;
}
        
// Removes a filter graph from the Running Object Table
void CTSFileSourceFilter::RemoveGraphFromRot(DWORD pdwRegister)
{
    CComPtr <IRunningObjectTable> pROT;

    if (SUCCEEDED(GetRunningObjectTable(0, &pROT))) 
        pROT->Revoke(pdwRegister);
}

void CTSFileSourceFilter::set_ROTMode()
{
	if (m_bRotEnable)
	{
		if (GetFilterGraph() && !m_dwGraphRegister && FAILED(AddGraphToRot (GetFilterGraph(), &m_dwGraphRegister)))
			m_dwGraphRegister = 0;
	}
	else if (m_dwGraphRegister)
	{
			RemoveGraphFromRot(m_dwGraphRegister);
			m_dwGraphRegister = 0;
	}
}

HRESULT CTSFileSourceFilter::GetObjectFromROT(WCHAR* wsFullName, IUnknown **ppUnk)
{
	if( *ppUnk )
		return E_FAIL;

	HRESULT	hr;

	IRunningObjectTablePtr spTable;
	IEnumMonikerPtr	spEnum = NULL;
	_bstr_t	bstrtFullName;

	bstrtFullName = wsFullName;

	// Get the IROT interface pointer
	hr = GetRunningObjectTable( 0, &spTable ); 
	if (FAILED(hr))
		return E_FAIL;

	// Get the moniker enumerator
	hr = spTable->EnumRunning( &spEnum ); 
	if (SUCCEEDED(hr))
	{
		_bstr_t	bstrtCurName; 

		// Loop thru all the interfaces in the enumerator looking for our reqd interface 
		IMonikerPtr spMoniker = NULL;
		while (SUCCEEDED(spEnum->Next(1, &spMoniker, NULL)) && (NULL != spMoniker))
		{
			// Create a bind context 
			IBindCtxPtr spContext = NULL;
			hr = CreateBindCtx(0, &spContext); 
			if (SUCCEEDED(hr))
			{
				// Get the display name
				WCHAR *wsCurName = NULL;
				hr = spMoniker->GetDisplayName(spContext, NULL, &wsCurName );
				bstrtCurName = wsCurName;

				// We have got our required interface pointer //
				if (SUCCEEDED(hr) && bstrtFullName == bstrtCurName)
				{ 
					hr = spTable->GetObject( spMoniker, ppUnk );
					return hr;
				}	
			}
			spMoniker.Release();
		}
	}
	return E_FAIL;
}

HRESULT CTSFileSourceFilter::ShowEPGInfo()
{
	CAutoLock lock(&m_Lock);
	return showEPGInfo();
}

HRESULT CTSFileSourceFilter::showEPGInfo()
{
	HRESULT hr = m_pPidParser->get_EPGFromFile();
	if (hr == S_OK)
	{
		unsigned char netname[128] = "";
		unsigned char onetname[128] ="";
		unsigned char chname[128] ="";
		unsigned char chnumb[128] ="";
		unsigned char shortdescripor[128] ="";
		unsigned char Extendeddescripor[600] ="";
		unsigned char shortnextdescripor[128] ="";
		unsigned char Extendednextdescripor[600] ="";
		m_pPidParser->get_NetworkName((unsigned char*)&netname);
		m_pPidParser->get_ONetworkName((unsigned char*)&onetname);
		m_pPidParser->get_ChannelName((unsigned char*)&chname);
		m_pPidParser->get_ChannelNumber((unsigned char*)&chnumb);
		m_pPidParser->get_ShortDescr((unsigned char*)&shortdescripor);
		m_pPidParser->get_ExtendedDescr((unsigned char*)&Extendeddescripor);
		m_pPidParser->get_ShortNextDescr((unsigned char*)&shortnextdescripor);
		m_pPidParser->get_ExtendedNextDescr((unsigned char*)&Extendednextdescripor);
		TCHAR szBuffer[(6*128)+ (2*600)];
		sprintf(szBuffer, "Network Name:- %s\n"
		"ONetwork Name:- %s\n"
		"Channel Number:- %s\n"
		"Channel Name:- %s\n\n"
		"Program Name: - %s\n"
		"Program Description:- %s\n\n"
		"Next Program Name: - %s\n"
		"Next Program Description:- %s\n"
			,netname,
			onetname,
			chnumb,
			chname,
			shortdescripor,
			Extendeddescripor,
			shortnextdescripor,
			Extendednextdescripor
			);
			MessageBox(NULL, szBuffer, TEXT("Program Infomation"), MB_OK);
	}
	return hr;
}

STDMETHODIMP CTSFileSourceFilter::GetPCRPosition(REFERENCE_TIME *pos)
{
	if(!pos)
		return E_INVALIDARG;

	CAutoLock lock(&m_Lock);
	*pos = m_pPin->getPCRPosition();

	return NOERROR;

}

STDMETHODIMP CTSFileSourceFilter::ShowStreamMenu(HWND hwnd)
{
	CAutoLock lock(&m_Lock);
	HRESULT hr;

	POINT mouse;
	GetCursorPos(&mouse);

	HMENU hMenu = CreatePopupMenu();
	if (hMenu)
	{
		IAMStreamSelect *pIAMStreamSelect;
		hr = this->QueryInterface(IID_IAMStreamSelect, (void**)&pIAMStreamSelect);
		if (SUCCEEDED(hr))
		{
			ULONG count;
			pIAMStreamSelect->Count(&count);

			ULONG flags, group, lastgroup = -1;
				
			for(UINT i = 0; i < count; i++)
			{
				WCHAR* pStreamName = NULL;

				if(S_OK == pIAMStreamSelect->Info(i, 0, &flags, 0, &group, &pStreamName, 0, 0))
				{
					if(lastgroup != group && i) 
						::AppendMenu(hMenu, MF_SEPARATOR, NULL, NULL);

					lastgroup = group;

					if(pStreamName)
					{
//						UINT uFlags = MF_STRING | MF_ENABLED;
//						if (flags & AMSTREAMSELECTINFO_EXCLUSIVE)
//							uFlags |= MF_CHECKED; //MFT_RADIOCHECK;
//						else if (flags & AMSTREAMSELECTINFO_ENABLED)
//							uFlags |= MF_CHECKED;
						
						UINT uFlags = (flags?MF_CHECKED:MF_UNCHECKED) | MF_STRING | MF_ENABLED;
						::AppendMenuW(hMenu, uFlags, (i + 0x100), LPCWSTR(pStreamName));
						CoTaskMemFree(pStreamName);
					}
				}
			}

			SetForegroundWindow(hwnd);
			UINT index = ::TrackPopupMenu(hMenu, TPM_LEFTBUTTON|TPM_RETURNCMD, mouse.x, mouse.y, 0, hwnd, 0);
			PostMessage(hwnd, NULL, 0, 0);

			if(index & 0x100) 
				pIAMStreamSelect->Enable((index & 0xff), AMSTREAMSELECTENABLE_ENABLE);

			pIAMStreamSelect->Release();
		}
		DestroyMenu(hMenu);
	}
	return hr;
}























//*****************************************************************************************
//ASync Additions

STDMETHODIMP CTSFileSourceFilter::RequestAllocator(
                      IMemAllocator* pPreferred,
                      ALLOCATOR_PROPERTIES* pProps,
                      IMemAllocator ** ppActual)
{
	CAutoLock cObjectLock(m_pLock);
	Pause();
	Stop();

    return S_OK;
}

STDMETHODIMP CTSFileSourceFilter::Request(
                     IMediaSample* pSample,
                     DWORD_PTR dwUser)
{
	return E_NOTIMPL;
}

STDMETHODIMP CTSFileSourceFilter::WaitForNext(
                      DWORD dwTimeout,
                      IMediaSample** ppSample,  
                      DWORD_PTR * pdwUser)
{
	return E_NOTIMPL;
}

STDMETHODIMP CTSFileSourceFilter::SyncReadAligned(
                      IMediaSample* pSample)
{
	return E_NOTIMPL;
}

STDMETHODIMP CTSFileSourceFilter::SyncRead(
                      LONGLONG llPosition,  // absolute file position
                      LONG lLength,         // nr bytes required
                      BYTE* pBuffer)
{
    CheckPointer(pBuffer, E_POINTER);

	HRESULT hr;
	LONG dwBytesToRead = lLength;
    CAutoLock lck(&m_Lock);
    DWORD dwReadLength;

	if (m_pFileReader->IsFileInvalid())
	{
		hr = m_pFileReader->OpenFile();
		if (FAILED(hr))
			return E_FAIL;

		int count = 0;
		__int64 fileStart, fileSize = 0;
		m_pFileReader->GetFileSize(&fileStart, &fileSize);

		//If this a file start then return null.
		while(fileSize < 500000 && count < 10)
		{
			Sleep(100);
			m_pFileReader->GetFileSize(&fileStart, &fileSize);
			count++;
		}
	}

	__int64 fileStart, fileSize = 0;
	m_pFileReader->GetFileSize(&fileStart, &fileSize);
	// Read the data from the file
	llPosition = min(fileSize, llPosition);
	llPosition = max(0, llPosition);
//	hr = m_pFileReader->Read(pBuffer, dwBytesToRead, &dwReadLength, (__int64)(llPosition - fileSize), FILE_END);
	hr = m_pFileReader->Read(pBuffer, dwBytesToRead, &dwReadLength, llPosition, FILE_BEGIN);
	if (FAILED(hr))
		return hr;

	if (dwReadLength < (DWORD)dwBytesToRead) 
	{
		WORD wReadOnly = 0;
		m_pFileReader->get_ReadOnly(&wReadOnly);
		if (wReadOnly)
		{
			while (dwReadLength < (DWORD)dwBytesToRead) 
			{
				WORD bDelay = 0;
				m_pFileReader->get_DelayMode(&bDelay);

				if (bDelay > 0)
					Sleep(2000);
				else
					Sleep(100);

				__int64 fileStart, filelength;
				m_pFileReader->GetFileSize(&fileStart, &filelength);
				ULONG ulNextBytesRead = 0;				
				llPosition = min(filelength, llPosition);
				llPosition = max(0, llPosition);
//				HRESULT hr = m_pFileReader->Read(pBuffer, dwBytesToRead, &dwReadLength, (__int64)(llPosition - filelength), FILE_END);
				HRESULT hr = m_pFileReader->Read(pBuffer, dwBytesToRead, &dwReadLength, llPosition, FILE_BEGIN);
				if (FAILED(hr))
					return hr;

				if ((ulNextBytesRead == 0) || (ulNextBytesRead == dwReadLength))
					return E_FAIL;

				dwReadLength = ulNextBytesRead;
			}
		}
		else
		{
			m_pFileReader->CloseFile();
			return E_FAIL;
		}
	}

	return NOERROR;
}

    // return total length of stream, and currently available length.
    // reads for beyond the available length but within the total length will
    // normally succeed but may block for a long period.
STDMETHODIMP CTSFileSourceFilter::Length(
                      LONGLONG* pTotal,
                      LONGLONG* pAvailable)
{
    CAutoLock lck(&m_Lock);

    CheckPointer(pTotal, E_POINTER);
    CheckPointer(pAvailable, E_POINTER);


	HRESULT hr;

	__int64 fileStart;
	__int64	fileSize = 0;

	if (m_pFileReader->IsFileInvalid())
	{
		hr = m_pFileReader->OpenFile();
		if (FAILED(hr))
			return E_FAIL;

		int count = 0;
		m_pFileReader->GetFileSize(&fileStart, &fileSize);

		//If this a file start then return null.
		while(fileSize < 500000 && count < 10)
		{
			Sleep(100);
			m_pFileReader->GetFileSize(&fileStart, &fileSize);
			count++;
		}
	}

	m_pFileReader->GetFileSize(&fileStart, &fileSize);

	*pTotal = fileSize;		
	*pAvailable = fileSize;		
	return NOERROR;
}

STDMETHODIMP CTSFileSourceFilter::BeginFlush(void)
{
	return E_NOTIMPL;
}

STDMETHODIMP CTSFileSourceFilter::EndFlush(void)
{
	return E_NOTIMPL;
}
//m_pPin->PrintTime(TEXT("Run"), (__int64) tStart, 10000);

//*****************************************************************************************


//////////////////////////////////////////////////////////////////////////
// End of interface implementations
//////////////////////////////////////////////////////////////////////////

