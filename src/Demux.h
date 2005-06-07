/**
*  Demux.h
*  Copyright (C) 2004-2005 bear
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
*  bear can be reached on the forums at
*    http://forums.dvbowners.com/
*/

#ifndef DEMUX_H
#define DEMUX_H

#include "PidParser.h"
#include "Control.h"

class Demux
{
public:

	Demux(PidParser *pPidParser);

//*********************************************************************************************
//Bug fix
	virtual ~Demux();

	STDMETHODIMP AOnConnect(IFilterGraph *pGraph);

	BOOL get_Auto();
	void set_Auto(BOOL bAuto);

//*********************************************************************************************
//NP Control Additions

	BOOL get_NPControl();
	void set_NPControl(BOOL bNPControl);

//NP Slave Additions

	BOOL get_NPSlave();
	void set_NPSlave(BOOL bNPSlave);

//Stop TIF Additions

	HRESULT SetTIFState(IFilterGraph *pGraph, REFERENCE_TIME tStart);

//*********************************************************************************************

	BOOL get_AC3Mode();
	void set_AC3Mode(BOOL bAC3Mode);

	BOOL get_CreateTSPinOnDemux();
	void set_CreateTSPinOnDemux(BOOL bCreateTSPinOnDemux);

	BOOL get_MPEG2AudioMediaType();
	void set_MPEG2AudioMediaType(BOOL bMPEG2AudioMediaType);

//**********************************************************************************************
//Audio2 Additions

	BOOL get_MPEG2Audio2Mode();
	void set_MPEG2Audio2Mode(BOOL bMPEG2Audio2Mode);
	int  get_MP2AudioPid();
	int Demux::get_AC3_2AudioPid();

//**********************************************************************************************

protected:
	HRESULT UpdateDemuxPins(IBaseFilter* pDemux);
	HRESULT CheckDemuxPin(IBaseFilter* pDemux, AM_MEDIA_TYPE pintype, IPin** pIPin);
	HRESULT CheckVideoPin(IBaseFilter* pDemux);
	HRESULT CheckAudioPin(IBaseFilter* pDemux);
	HRESULT CheckAC3Pin(IBaseFilter* pDemux);
	HRESULT CheckTelexPin(IBaseFilter* pDemux);
	HRESULT CheckTsPin(IBaseFilter* pDemux);
	HRESULT	NewTsPin(IMpeg2Demultiplexer* muxInterface, LPWSTR pinName);
	HRESULT	NewVideoPin(IMpeg2Demultiplexer* muxInterface, LPWSTR pinName);
	HRESULT	NewAudioPin(IMpeg2Demultiplexer* muxInterface, LPWSTR pinName);
	HRESULT	NewAC3Pin(IMpeg2Demultiplexer* muxInterface, LPWSTR pinName);
	HRESULT	NewTelexPin(IMpeg2Demultiplexer* muxInterface, LPWSTR pinName);
	HRESULT	LoadTsPin(IPin* pIPin);
	HRESULT	LoadMediaPin(IPin* pIPin, ULONG pid);
	HRESULT	LoadTelexPin(IPin* pIPin, ULONG pid);
	HRESULT	ClearDemuxPin(IPin* pIPin);
	HRESULT	ChangeDemuxPin(IBaseFilter* pDemux, LPWSTR* pPinName, BOOL* pConnect);
	HRESULT	GetAC3Media(AM_MEDIA_TYPE *pintype);
	HRESULT	GetMP2Media(AM_MEDIA_TYPE *pintype);
	HRESULT	GetMP1Media(AM_MEDIA_TYPE *pintype);
	HRESULT	GetVideoMedia(AM_MEDIA_TYPE *pintype);
	HRESULT	GetTelexMedia(AM_MEDIA_TYPE *pintype);
	HRESULT	GetTSMedia(AM_MEDIA_TYPE *pintype);
	HRESULT	Sleeps(ULONG Duration, long TimeOut[]);
	HRESULT	IsStopped();
	HRESULT	IsPlaying();
	HRESULT	IsPaused();
	HRESULT	DoStop();
	HRESULT	DoStart();
	HRESULT	DoPause();

//**********************************************************************************************
//NP Control Additions

	HRESULT UpdateNetworkProvider(IBaseFilter* pNetworkProvider);

//TIF Additions

	HRESULT CheckTIFPin(IBaseFilter* pDemux);
	HRESULT GetTIFMedia(AM_MEDIA_TYPE *pintype);

//**********************************************************************************************

protected:
	PidParser *m_pPidParser;

	BOOL   m_bAuto;

//*********************************************************************************************
//Bug fix

	bool	OnConnectBusyFlag;

//NP Control Additions

	BOOL 	m_bNPControl;

//NP Slave Additions

	BOOL 	m_bNPSlave;

//*********************************************************************************************

	BOOL   m_bAC3Mode;
	BOOL   m_bCreateTSPinOnDemux;
	BOOL   m_bMPEG2AudioMediaType;

//**********************************************************************************************
//Audio2 Additions

	BOOL m_bMPEG2Audio2Mode;

//**********************************************************************************************

	LONG   m_TimeOut[1];
	BOOL   m_WasPlaying;

	IGraphBuilder * m_pGraphBuilder;
	IMediaControl * m_pMediaControl;
	IFilterChain * m_pFilterChain;

};

#endif