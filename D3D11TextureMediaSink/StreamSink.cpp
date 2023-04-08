#include "stdafx.h"

namespace D3D11TextureMediaSink
{
	// static

	//  List of preferred video formats
	GUID const* const StreamSink::s_pVideoFormats[] =
	{
		&MFVideoFormat_NV12,
		//&MFVideoFormat_IYUV,
		&MFVideoFormat_YUY2,
		//&MFVideoFormat_YV12,
		&MFVideoFormat_RGB32,
		&MFVideoFormat_ARGB32,
		//&MFVideoFormat_RGB24,
		//&MFVideoFormat_RGB555,
		//&MFVideoFormat_RGB565,
		//&MFVideoFormat_RGB8,
		&MFVideoFormat_AYUV,
		//&MFVideoFormat_UYVY,
		//&MFVideoFormat_YVYU,
		//&MFVideoFormat_YVU9,
		//&MFVideoFormat_v216,
		//&MFVideoFormat_v410,
		//&MFVideoFormat_I420,
		&MFVideoFormat_NV11,
		&MFVideoFormat_420O
	};
	// Number of elements in the priority video format list
	const DWORD StreamSink::s_dwNumVideoFormats = sizeof(StreamSink::s_pVideoFormats) / sizeof(StreamSink::s_pVideoFormats[0]);

	// Mapping table between video formats and DXGI formats
	const StreamSink::FormatEntry StreamSink::s_DXGIFormatMapping[] =
	{
		{ MFVideoFormat_RGB32, DXGI_FORMAT_B8G8R8X8_UNORM },
		{ MFVideoFormat_ARGB32, DXGI_FORMAT_R8G8B8A8_UNORM },
		{ MFVideoFormat_AYUV, DXGI_FORMAT_AYUV },
		{ MFVideoFormat_YUY2, DXGI_FORMAT_YUY2 },
		{ MFVideoFormat_NV12, DXGI_FORMAT_NV12 },
		{ MFVideoFormat_NV11, DXGI_FORMAT_NV11 },
		{ MFVideoFormat_AI44, DXGI_FORMAT_AI44 },
		{ MFVideoFormat_P010, DXGI_FORMAT_P010 },
		{ MFVideoFormat_P016, DXGI_FORMAT_P016 },
		{ MFVideoFormat_Y210, DXGI_FORMAT_Y210 },
		{ MFVideoFormat_Y216, DXGI_FORMAT_Y216 },
		{ MFVideoFormat_Y410, DXGI_FORMAT_Y410 },
		{ MFVideoFormat_Y416, DXGI_FORMAT_Y416 },
		{ MFVideoFormat_420O, DXGI_FORMAT_420_OPAQUE },
		{ GUID_NULL, DXGI_FORMAT_UNKNOWN }
	};

	// Operation detection map
	BOOL StreamSink::ValidStateMatrix[StreamSink::State_Count][StreamSink::Op_Count] =
	{
		// States:    Operations:
		//            SetType   Start     Restart   Pause     Stop      Sample    Marker
		/* NotSet */  TRUE, FALSE, FALSE, FALSE, FALSE, FALSE, FALSE,

		/* Ready */   TRUE, TRUE, TRUE, TRUE, TRUE, FALSE, TRUE,

		/* Start */   TRUE, TRUE, FALSE, TRUE, TRUE, TRUE, TRUE,

		/* Pause */   TRUE, TRUE, TRUE, TRUE, TRUE, TRUE, TRUE,

		/* Stop */    TRUE, TRUE, FALSE, FALSE, TRUE, FALSE, TRUE

		// Note about states:
		// 1. OnClockRestart should only be called from paused state.
		// 2. While paused, the sink accepts samples but does not process them.
	};

	const MFRatio StreamSink::s_DefaultFrameRate = { 30, 1 };

	// Control how we batch work from the decoder.
	// On receiving a sample we request another one if the number on the queue is
	// less than the hi water threshold.
	// When displaying samples (removing them from the sample queue) we request
	// another one if the number of falls below the lo water threshold
	//
#define SAMPLE_QUEUE_HIWATER_THRESHOLD 3
	// maximum # of past reference frames required for deinterlacing
#define MAX_PAST_FRAMES         3


	// method

	StreamSink::StreamSink(IMFMediaSink* pParentMediaSink, CriticalSection* critsec, Scheduler* pScheduler, Presenter* pPresenter) :
		_WorkQueueCB(this, &StreamSink::OnDispatchWorkItem)
	{
		this->Initialize();

		this->_ReferenceCount = 1;
		this->_ShutdownFlag = FALSE;
		this->_PreprocessingQueue = new ThreadSafeComPtrQueue<IUnknown>();
		this->_ParentMediaSink = pParentMediaSink;
		this->_csStreamSinkAndScheduler = critsec;
		this->_csPresentedSample = new CriticalSection();
		this->_Scheduler = pScheduler;
		this->_Presenter = pPresenter;

		::MFAllocateWorkQueueEx(MF_STANDARD_WORKQUEUE, &this->m_WorkQueueId);

		// Create an event queue.
		::MFCreateEventQueue(&this->_EventQueue);
	}
	StreamSink::~StreamSink()
	{
	}

	HRESULT StreamSink::Start(MFTIME start, IMFPresentationClock* pClock)
	{
		//OutputDebugString(L"StreamSink::Start\n");

		AutoLock lock(this->_csStreamSinkAndScheduler);

		HRESULT hr = S_OK;

		do
		{
			// Is the operation valid?
			if (FAILED(hr = this->ValidateOperation(OpStart)))
				break;

			if (start != PRESENTATION_CURRENT_POSITION)
			{
				// We're starting from a "new" position
				this->_StartTime = start;        // Cache the start time.
			}

			// Update the presentation clock.
			SafeRelease(this->_PresentationClock);
			this->_PresentationClock = pClock;
			if (NULL != this->_PresentationClock)
				this->_PresentationClock->AddRef();

			// Execute the asynchronous operation Start.
			this->_State = State_Started;
			hr = this->QueueAsyncOperation(OpStart);

		} while (FALSE);

		this->_WaitingForOnClockStart = FALSE;

		return hr;
	}
	HRESULT StreamSink::Pause()
	{
		//OutputDebugString(L"StreamSink::Pause\n");

		AutoLock lock(this->_csStreamSinkAndScheduler);

		HRESULT hr;

		// Is operation permitted?
		if (FAILED(hr = ValidateOperation(OpPause)))
			return hr;

		// Perform synchronous operation Pause
		this->_State = State_Paused;
		return this->QueueAsyncOperation(OpPause);
	}
	HRESULT StreamSink::Restart()
	{
		//OutputDebugString(L"StreamSink::Restart\n");

		AutoLock lock(this->_csStreamSinkAndScheduler);

		HRESULT hr;

		// Is operation permitted?
		if (FAILED(hr = ValidateOperation(OpRestart)))
			return hr;

		// Perform asynchronous operation Restart
		this->_State = State_Started;
		return this->QueueAsyncOperation(OpRestart);
	}
	HRESULT StreamSink::Stop()
	{
		//OutputDebugString(L"StreamSink::Stop\n");

		AutoLock lock(this->_csStreamSinkAndScheduler);

		HRESULT hr;

		// Is operation valid?
		if (FAILED(hr = ValidateOperation(OpStop)))
			return hr;

		// Release presentation clock.
		SafeRelease(this->_PresentationClock);

		// Execute asynchronous operation Stop.
		this->_State = State_Stopped;
		return this->QueueAsyncOperation(OpStop);
	}
	HRESULT StreamSink::Shutdown()
	{
		//OutputDebugString(L"StreamSink::Shutdown\n");

		AutoLock(this->_csStreamSinkAndScheduler);

		this->_EventQueue->Shutdown();
		SafeRelease(this->_EventQueue);

		::MFUnlockWorkQueue(this->m_WorkQueueId);

		this->_PreprocessingQueue->Clear();

		SafeRelease(this->_PresentationClock);

		this->_ParentMediaSink = NULL;
		this->_Presenter = NULL;
		this->_Scheduler = NULL;
		this->_CurrentType = NULL;

		return S_OK;
	}

	void StreamSink::LockPresentedSample(IMFSample** ppSample)
	{
		this->_csPresentedSample->Lock();
		*ppSample = this->_PresentedSample;
		if (*ppSample != NULL)
			(*ppSample)->AddRef();
	}
	void StreamSink::UnlockPresentedSample()
	{
		this->_csPresentedSample->Unlock();
	}

	// IUnknown Implementation

	ULONG	StreamSink::AddRef()
	{
		return InterlockedIncrement(&this->_ReferenceCount);
	}
	HRESULT StreamSink::QueryInterface(REFIID iid, __RPC__deref_out _Result_nullonfailure_ void** ppv)
	{
		if (NULL == ppv)
			return E_POINTER;

		if (iid == IID_IUnknown)
		{
			*ppv = static_cast<IUnknown*>(static_cast<IMFStreamSink*>(this));
		}
		else if (iid == __uuidof(IMFStreamSink))
		{
			*ppv = static_cast<IMFStreamSink*>(this);
		}
		else if (iid == __uuidof(IMFMediaEventGenerator))
		{
			*ppv = static_cast<IMFMediaEventGenerator*>(this);
		}
		else if (iid == __uuidof(IMFMediaTypeHandler))
		{
			*ppv = static_cast<IMFMediaTypeHandler*>(this);
		}
		else if (iid == __uuidof(IMFGetService))
		{
			*ppv = static_cast<IMFGetService*>(this);
		}
		else if (iid == __uuidof(IMFAttributes))
		{
			*ppv = static_cast<IMFAttributes*>(this);
		}
		else
		{
			*ppv = NULL;
			return E_NOINTERFACE;
		}

		this->AddRef();

		return S_OK;
	}
	ULONG	StreamSink::Release()
	{
		ULONG uCount = InterlockedDecrement(&this->_ReferenceCount);

		if (uCount == 0)
			delete this;

		return uCount;
	}

	// IMFStreamSink Implementation

	HRESULT	StreamSink::Flush()
	{
		//OutputDebugString(L"StreamSink::Flush\n");

		AutoLock lock(this->_csStreamSinkAndScheduler);

		HRESULT hr;

		do
		{
			if (FAILED(hr = this->CheckShutdown()))
				break;

			this->_ConsumeData = DropFrames;
			// Note: Even though we are flushing data, we still need to send
			// any marker events that were queued.
			if (FAILED(hr = this->ProcessSamplesFromQueue(this->_ConsumeData)))
				break;

			hr = this->_Scheduler->Flush();	// This call blocks until the scheduler threads discards all scheduled samples.
			hr = this->_Presenter->Flush();

		} while (FALSE);

		this->_ConsumeData = ProcessFrames;

		return hr;
	}
	HRESULT StreamSink::GetIdentifier(__RPC__out DWORD* pdwIdentifier)
	{
		//OutputDebugString(L"StreamSink::GetIdentifier\n");

		AutoLock lock(this->_csStreamSinkAndScheduler);

		if (pdwIdentifier == NULL)
			return E_POINTER;

		HRESULT hr;

		// Is shutdown done?
		if (FAILED(hr = this->CheckShutdown()))
			return hr;

		// Return stream ID.
		*pdwIdentifier = 0;	// Fixed.

		return hr;
	}
	HRESULT StreamSink::GetMediaSink(__RPC__deref_out_opt IMFMediaSink** ppMediaSink)
	{
		//OutputDebugString(L"StreamSink::GetMediaSink\n");

		AutoLock lock(this->_csStreamSinkAndScheduler);

		HRESULT hr;

		if (ppMediaSink == NULL)
			return E_POINTER;

		// Is it already shut down?
		if (FAILED(hr = this->CheckShutdown()))
			return hr;

		*ppMediaSink = this->_ParentMediaSink;
		(*ppMediaSink)->AddRef();

		return S_OK;
	}
	HRESULT StreamSink::GetMediaTypeHandler(__RPC__deref_out_opt IMFMediaTypeHandler** ppHandler)
	{
		//OutputDebugString(L"StreamSink::GetMediaTypeHandler\n");

		AutoLock lock(this->_csStreamSinkAndScheduler);

		if (ppHandler == NULL)
			return E_POINTER;

		HRESULT hr;

		if (FAILED(hr = this->CheckShutdown()))
			return hr;

		// This stream sink also acts as its own type handler, so query itself for the IID_IMFMediaTypeHandler interface.
		return this->QueryInterface(IID_IMFMediaTypeHandler, (void**)ppHandler);
	}
	HRESULT StreamSink::PlaceMarker(MFSTREAMSINK_MARKER_TYPE eMarkerType, __RPC__in const PROPVARIANT* pvarMarkerValue, __RPC__in const PROPVARIANT* pvarContextValue)
	{
		//OutputDebugString(L"StreamSink::PlaceMarker\n");

		AutoLock lock(this->_csStreamSinkAndScheduler);

		HRESULT hr = S_OK;
		IMarker* pMarker = NULL;

		do
		{
			// Is it already shut down?
			if (FAILED(hr = this->CheckShutdown()))
				break;

			// Check if the operation is valid.
			if (FAILED(hr = this->ValidateOperation(OpPlaceMarker)))
				break;

			// Create a marker and add it to the pre-processing queue.
			if (FAILED(hr = Marker::Create(eMarkerType, pvarMarkerValue, pvarContextValue, &pMarker)))
				break;
			if (FAILED(hr = this->_PreprocessingQueue->Queue(pMarker)))
				break;

			// If the stream is not paused, queue an asynchronous operation to process the marker/sample.
			if (this->_State != State_Paused)
			{
				if (FAILED(hr = this->QueueAsyncOperation(OpPlaceMarker)))
					break;
			}

		} while (FALSE);

		SafeRelease(pMarker);

		return hr;
	}
	HRESULT StreamSink::ProcessSample(__RPC__in_opt IMFSample* pSample)
	{
		HRESULT hr = S_OK;

		//OutputDebugString(L"StreamSink::ProcessSample\n");

		AutoLock lock(this->_csStreamSinkAndScheduler);

		if (NULL == pSample)
			return E_POINTER;

		if (0 == this->_OutstandingSampleRequests)
			return MF_E_INVALIDREQUEST;	// Nobody is requesting samples


		// Has the object been shut down?
		if (FAILED(hr = this->CheckShutdown()))
			return hr;

		this->_OutstandingSampleRequests--;

		// Can the operation be performed?
		if (!this->_WaitingForOnClockStart)
		{
			if (FAILED(hr = this->ValidateOperation(OpProcessSample)))
				return hr;
		}

		// Add the sample to the pre-processing queue.
		if (FAILED(hr = this->_PreprocessingQueue->Queue(pSample)))
			return hr;

		// If not paused or stopped, perform the asynchronous processSample operation.
		if (this->_State != State_Paused && this->_State != State_Stopped)
		{
			if (FAILED(hr = this->QueueAsyncOperation(OpProcessSample)))
				return hr;
		}

		return S_OK;
	}

	// IMFMediaEventGenerator (in IMFStreamSink) Implementation
	HRESULT StreamSink::BeginGetEvent(IMFAsyncCallback* pCallback, IUnknown* punkState)
	{
		AutoLock lock(this->_csStreamSinkAndScheduler);

		HRESULT hr;

		// Is shutdown completed?
		if (FAILED(hr = this->CheckShutdown()))
			return hr;

		// Delegate to the event queue.
		return this->_EventQueue->BeginGetEvent(pCallback, punkState);
	}
	HRESULT StreamSink::EndGetEvent(IMFAsyncResult* pResult, _Out_ IMFMediaEvent** ppEvent)
	{
		AutoLock lock(this->_csStreamSinkAndScheduler);

		HRESULT hr;

		// Is shutdown completed?
		if (FAILED(hr = this->CheckShutdown()))
			return hr;

		// Delegate to the event queue.
		return this->_EventQueue->EndGetEvent(pResult, ppEvent);
	}
	HRESULT StreamSink::GetEvent(DWORD dwFlags, __RPC__deref_out_opt IMFMediaEvent** ppEvent)
	{
		// NOTE:
		// GetEvent can block indefinitely, so we don't hold the lock.
		// This requires some juggling with the event queue pointer.

		HRESULT hr;
		IMFMediaEventQueue* pQueue = NULL;

		do
		{
			// scope for lock
			{
				AutoLock lock(this->_csStreamSinkAndScheduler);

				// Is shutdown completed?
				if (FAILED(hr = this->CheckShutdown()))
					break;

				// Get the pointer to the event queue.
				pQueue = this->_EventQueue;
				pQueue->AddRef();
			}

			// Now get the event.
			hr = pQueue->GetEvent(dwFlags, ppEvent);

		} while (FALSE);

		SafeRelease(pQueue);

		return hr;
	}
	HRESULT StreamSink::QueueEvent(MediaEventType met, __RPC__in REFGUID guidExtendedType, HRESULT hrStatus, __RPC__in_opt const PROPVARIANT* pvValue)
	{
		//OutputDebugString(L"StreamSink::QueueEvent\n");

		AutoLock lock(this->_csStreamSinkAndScheduler);

		HRESULT hr;

		// Is already shutdown?
		if (FAILED(hr = this->CheckShutdown()))
			return hr;

		// Delegate to the event queue.
		return this->_EventQueue->QueueEventParamVar(met, guidExtendedType, hrStatus, pvValue);
	}

	// IMFMediaTypeHandler Implementation

	HRESULT StreamSink::GetCurrentMediaType(_Outptr_ IMFMediaType** ppMediaType)
	{
		//OutputDebugString(L"StreamSink::GetCurrentMediaType\n");

		AutoLock lock(this->_csStreamSinkAndScheduler);

		if (ppMediaType == NULL)
			return E_POINTER;

		HRESULT hr;

		// Is already shutdown?
		if (FAILED(hr = this->CheckShutdown()))
			return hr;

		if (NULL == this->_CurrentType)
			return MF_E_NOT_INITIALIZED;

		*ppMediaType = this->_CurrentType;
		(*ppMediaType)->AddRef();

		return S_OK;
	}
	HRESULT StreamSink::GetMajorType(__RPC__out GUID* pguidMajorType)
	{
		//OutputDebugString(L"StreamSink::GetMajorType\n");

		if (NULL == pguidMajorType)
			return E_POINTER;

		HRESULT hr;

		// Shutdown completed?
		if (FAILED(hr = this->CheckShutdown()))
			return hr;

		// Media type not set?
		if (NULL == this->_CurrentType)
			return MF_E_NOT_INITIALIZED;

		// Return the major type of the current media type.
		return this->_CurrentType->GetGUID(MF_MT_MAJOR_TYPE, pguidMajorType);
	}
	HRESULT StreamSink::GetMediaTypeByIndex(DWORD dwIndex, _Outptr_ IMFMediaType** ppType)
	{
		//OutputDebugString(L"StreamSink::GetMediaTypeByIndex\n");

		HRESULT hr = S_OK;

		if (NULL == ppType)
			return E_POINTER;

		// Shutdown completed?
		if (FAILED(hr = this->CheckShutdown()))
			return hr;

		if (dwIndex >= s_dwNumVideoFormats)
			return MF_E_NO_MORE_TYPES;	// Index out of range

		IMFMediaType* pVideoMediaType = NULL;

		// Create and return the media type corresponding to the dwIndex-th preferred video format.
		do
		{
			if (FAILED(hr = ::MFCreateMediaType(&pVideoMediaType)))
				break;

			if (FAILED(hr = pVideoMediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)))
				break;

			if (FAILED(hr = pVideoMediaType->SetGUID(MF_MT_SUBTYPE, *(s_pVideoFormats[dwIndex]))))
				break;

			pVideoMediaType->AddRef();
			*ppType = pVideoMediaType;

		} while (FALSE);

		SafeRelease(pVideoMediaType);

		return S_OK;
	}
	HRESULT StreamSink::GetMediaTypeCount(__RPC__out DWORD* pdwTypeCount)
	{
		//OutputDebugString(L"StreamSink::GetMediaTypeCount\n");

		HRESULT hr = S_OK;

		if (NULL == pdwTypeCount)
			return E_POINTER;

		// Shutdown completed?
		if (FAILED(hr = this->CheckShutdown()))
			return hr;

		// Return the number of preferred video formats (i.e., the number of preferred media types).
		*pdwTypeCount = s_dwNumVideoFormats;

		return S_OK;
	}
	HRESULT StreamSink::IsMediaTypeSupported(IMFMediaType* pMediaType, _Outptr_opt_result_maybenull_ IMFMediaType** ppMediaType)
	{
		//OutputDebugString(L"StreamSink::IsMediaTypeSuppored\n");

		HRESULT hr = S_OK;
		GUID subType = GUID_NULL;

		// We don't return any "close match" types.
		if (ppMediaType)
			*ppMediaType = NULL;

		// Shutdown completed?
		if (FAILED(hr = this->CheckShutdown()))
			return hr;

		if (NULL == pMediaType)
			return E_POINTER;

		// Get the subtype of the media type to be checked.
		if (FAILED(hr = pMediaType->GetGUID(MF_MT_SUBTYPE, &subType)))
			return hr;

		// Is it in the preferred format list?
		hr = MF_E_INVALIDMEDIATYPE;
		for (DWORD i = 0; i < s_dwNumVideoFormats; i++)
		{
			if (subType == (*s_pVideoFormats[i]))
			{
				hr = S_OK;
				break;
			}
		}
		if (FAILED(hr))
			return hr;		// Not found; preferred format not supported

		// Get the corresponding DXGI format.
		DWORD i = 0;
		while (s_DXGIFormatMapping[i].Subtype != GUID_NULL)
		{
			const FormatEntry& e = s_DXGIFormatMapping[i];
			if (e.Subtype == subType)
			{
				this->_dxgiFormat = e.DXGIFormat;
				break;
			}
			i++;
		}

		// Is it supported by the presenter?
		if (FAILED(hr = this->_Presenter->IsSupported(pMediaType, this->_dxgiFormat)))
			return hr;

		return S_OK;
	}
	HRESULT StreamSink::SetCurrentMediaType(IMFMediaType* pMediaType)
	{
		//OutputDebugString(L"StreamSink::SetCurrentMediaType\n");

		if (pMediaType == NULL)
			return E_POINTER;

		HRESULT hr = S_OK;

		AutoLock lock(this->_csStreamSinkAndScheduler);


		// Shutdown completed?
		if (FAILED(hr = this->CheckShutdown()))
			return hr;

		// Can operate?
		if (FAILED(hr = this->ValidateOperation(OpSetMediaType)))
			return hr;

		// Is the specified media type supported?
		if (FAILED(hr = this->IsMediaTypeSupported(pMediaType, NULL)))
			return hr;

		// Update the current media type to the specified media type.
		SafeRelease(this->_CurrentType);
		this->_CurrentType = pMediaType;
		this->_CurrentType->AddRef();

		// Get and save the interlace mode.
		hr = pMediaType->GetUINT32(MF_MT_INTERLACE_MODE, &this->_InterlaceMode);

		// Set the frame rate to the scheduler.
		MFRatio fps = { 0, 0 };
		if (SUCCEEDED(this->GetFrameRate(pMediaType, &fps)) && (fps.Numerator != 0) && (fps.Denominator != 0))
		{
			if (MFVideoInterlace_FieldInterleavedUpperFirst == this->_InterlaceMode ||
				MFVideoInterlace_FieldInterleavedLowerFirst == this->_InterlaceMode ||
				MFVideoInterlace_FieldSingleUpper == this->_InterlaceMode ||
				MFVideoInterlace_FieldSingleLower == this->_InterlaceMode ||
				MFVideoInterlace_MixedInterlaceOrProgressive == this->_InterlaceMode)
			{
				fps.Numerator *= 2;
			}
			this->_Scheduler->SetFrameRate(fps);
		}
		else
		{
			// NOTE: The mixer's proposed type might not have a frame rate, in which case
			// we'll use an arbitary default. (Although it's unlikely the video source
			// does not have a frame rate.)
			this->_Scheduler->SetFrameRate(s_DefaultFrameRate);
		}

		// Modify the required number of samples based on the media type (progressive or interlace).
		if (this->_InterlaceMode == MFVideoInterlace_Progressive)
		{
			// XVP will hold on to 1 sample but that's the same sample we will internally hold on to
			hr = this->SetUINT32(MF_SA_REQUIRED_SAMPLE_COUNT, SAMPLE_QUEUE_HIWATER_THRESHOLD);
		}
		else
		{
			// Assume we will need a maximum of 3 backward reference frames for deinterlacing
			// However, one of the frames is "shared" with SVR
			hr = this->SetUINT32(MF_SA_REQUIRED_SAMPLE_COUNT, SAMPLE_QUEUE_HIWATER_THRESHOLD + MAX_PAST_FRAMES - 1);
		}

		// Set the media type to the presenter.
		if (SUCCEEDED(hr))
		{
			if (FAILED(hr = this->_Presenter->SetCurrentMediaType(pMediaType)))
				return hr;
		}

		// If not starting or pausing, return to the ready status.
		if (State_Started != this->_State && State_Paused != this->_State)
		{
			this->_State = State_Ready;
		}
		else
		{
			// If starting or pausing, flush the samples in the queue as this is a format change.
			hr = this->Flush();
		}

		return hr;
	}

	// IMFGetService Implementation

	HRESULT StreamSink::GetService(__RPC__in REFGUID guidService, __RPC__in REFIID riid, __RPC__deref_out_opt LPVOID* ppvObject)
	{
		//OutputDebugString(L"StreamSink::GetService\n");

		HRESULT hr;

		if (guidService == MR_VIDEO_ACCELERATION_SERVICE)
		{
			if (riid == __uuidof(IMFDXGIDeviceManager))
			{
				*ppvObject = (void*) static_cast<IUnknown*>(this->_Presenter->GetDXGIDeviceManager());
				return S_OK;
			}
			else
			{
				return E_NOINTERFACE;
			}
		}
		else
		{
			return MF_E_UNSUPPORTED_SERVICE;
		}

		return hr;
	}

	// SchedulerCallback Implementation

	HRESULT StreamSink::PresentFrame(IMFSample* pSample)
	{
		//_OutputDebugString(L"StreamSink::PresentFrame\n");

		AutoLock lock(this->_csPresentedSample);

		HRESULT hr = S_OK;

		// Release the current sample.
		if (NULL != this->_PresentedSample)
			this->_Presenter->ReleaseSample(this->_PresentedSample);

		// Update the specified sample's SAMPLE_STATE to PRESENT.
		if (FAILED(hr = pSample->SetUINT32(SAMPLE_STATE, SAMPLE_STATE_PRESENT)))
			return hr;



		//LONGLONG time;
		//pSample->GetSampleTime(&time);
		//_OutputDebugString(_T("StreamSink::PresentFrame; %lld\n"), time);


		// Swap the samples.
		this->_PresentedSample = pSample;

		return S_OK;
	}


	// private

	HRESULT StreamSink::CheckShutdown() const
	{
		return (this->_ShutdownFlag) ? MF_E_SHUTDOWN : S_OK;
	}
	HRESULT StreamSink::GetFrameRate(IMFMediaType* pType, MFRatio* pRatio)
	{
		if (NULL == pRatio)
			return E_POINTER;

		// Get the frame rate of the specified media type.
		return ::MFGetAttributeRatio(pType, MF_MT_FRAME_RATE, (UINT32*)&pRatio->Numerator, (UINT32*)&pRatio->Denominator);
	}
	HRESULT StreamSink::ValidateOperation(StreamOperation op)
	{
		HRESULT hr = S_OK;

		BOOL bTransitionAllowed = ValidStateMatrix[this->_State][op];

		if (bTransitionAllowed)
		{
			return S_OK;
		}
		else
		{
			return MF_E_INVALIDREQUEST;
		}
	}

	HRESULT StreamSink::QueueAsyncOperation(StreamOperation op)
	{
		//OutputDebugString(L"StreamSink::QueueAsyncOperation\n");

		HRESULT hr = S_OK;

		AsyncOperation* pOp = new AsyncOperation(op);
		do
		{
			if (NULL == pOp)
			{
				hr = E_OUTOFMEMORY;
				break;
			}

			// Add an asynchronous item to the work queue.
			if (FAILED(hr = ::MFPutWorkItem(this->m_WorkQueueId, &this->_WorkQueueCB, pOp)))
				break;

		} while (FALSE);

		SafeRelease(pOp);

		return hr;
	}
	HRESULT StreamSink::OnDispatchWorkItem(IMFAsyncResult* pAsyncResult)
	{
		//OutputDebugString(L"StreamSink::OnDispatchWorkItem\n");

		AutoLock lock(this->_csStreamSinkAndScheduler);

		HRESULT hr;

		// Check if the stream sink has been shut down
		if (FAILED(hr = this->CheckShutdown()))
			return hr;


		IUnknown* pState = NULL;

		if (SUCCEEDED(hr = pAsyncResult->GetState(&pState)))
		{
			AsyncOperation* pOp = (AsyncOperation*)pState;
			StreamOperation op = pOp->m_op;

			switch (op)
			{
			case OpStart:
			case OpRestart:

				//_OutputDebugString(_T("OnStart/OnRestart\n"));

				// Queue the MEStreamSinkStarted event
				if (FAILED(hr = this->QueueEvent(MEStreamSinkStarted, GUID_NULL, hr, NULL)))
					break;

				// Increment the sample request count and queue the MEStreamSinkRequestSample event
				this->_OutstandingSampleRequests++;
				if (FAILED(hr = this->QueueEvent(MEStreamSinkRequestSample, GUID_NULL, hr, NULL)))
					break;

				// Process samples from the queue
				if (FAILED(hr = this->ProcessSamplesFromQueue(this->_ConsumeData)))
					break;
				break;

			case OpStop:

				// Flush the stream sink and reset the sample request count
				this->Flush();
				this->_OutstandingSampleRequests = 0;

				// Queue the MEStreamSinkStopped event
				if (FAILED(hr = this->QueueEvent(MEStreamSinkStopped, GUID_NULL, hr, NULL)))
					break;
				break;

			case OpPause:

				// Queue the MFStreamSinkPaused event
				if (FAILED(hr = this->QueueEvent(MEStreamSinkPaused, GUID_NULL, hr, NULL)))
					break;
				break;

			case OpProcessSample:
			case OpPlaceMarker:

				if (!(this->_WaitingForOnClockStart))
				{
					if (FAILED(hr = this->DispatchProcessSample(pOp)))
						break;
				}
				break;
			}

			SafeRelease(pState);
		}

		return hr;
	}
	HRESULT StreamSink::DispatchProcessSample(AsyncOperation *pOp)
	{
		//OutputDebugString(L"StreamSink::DispatchProcessSample\n");

		HRESULT hr = S_OK;

		// Is it already shut down?
		if (FAILED(hr = this->CheckShutdown()))
			return hr;

		// Can we process the next sample?
		if (this->_Presenter->IsReadyNextSample())
		{
			do
			{
				// Process the samples in the queue.
				if (FAILED(hr = this->ProcessSamplesFromQueue(ProcessFrames)))
					break;

				// Check if there are other samples.
				if (pOp->m_op == OpProcessSample)
				{
					if (FAILED(hr = this->RequestSamples()))
						break;
				}

			} while (FALSE);

			// If something fails during the asynchronous operation, send MEError to the client.
			if (FAILED(hr))
				hr = this->QueueEvent(MEError, GUID_NULL, hr, NULL);
		}

		return hr;
	}
	HRESULT StreamSink::ProcessSamplesFromQueue(ConsumeState bConsumeData)
	{
		//OutputDebugString(L"StreamSink::ProcessSamplesFromQueue\n");

		HRESULT hr = S_OK;
		IUnknown* pUnk = nullptr;
		BOOL bProcessMoreSamples = TRUE;
		BOOL bDeviceChanged = FALSE;
		BOOL bProcessAgain = FALSE;

		// About samples/markers in the queue...
		while (this->_PreprocessingQueue->Dequeue(&pUnk) == S_OK)	// If empty, Dequeue() returns S_FALSE.
		{
			bProcessMoreSamples = TRUE;
			IMarker* pMarker = NULL;
			IMFSample* pSample = NULL;
			IMFSample* pOutSample = NULL;

			do
			{
				// Check if pUnk is a marker or a sample.
				if ((hr = pUnk->QueryInterface(__uuidof(IMarker), (void**)&pMarker)) == E_NOINTERFACE)
				{
					if (FAILED(hr = pUnk->QueryInterface(IID_IMFSample, (void**)&pSample)))
						break;
				}

				// Process branching for markers and samples.

				if (pMarker)
				{
					// (A) When a marker is detected.

					HRESULT hrStatus = S_OK;  // Status code for marker event.

					PROPVARIANT var;
					::PropVariantInit(&var);

					if (this->_ConsumeData == DropFrames)
						hrStatus = E_ABORT;

					do
					{
						if (FAILED(hr = pMarker->GetContext(&var)))
							break;

						// Communicate the marker status to the client.
						if (FAILED(hr = this->QueueEvent(MEStreamSinkMarker, GUID_NULL, hrStatus, &var)))
							break;

					} while (FALSE);

					::PropVariantClear(&var);

					hr = S_OK;
					break;
				}
				else
				{
					// (B) When a sample is detected.

					if (bConsumeData == ProcessFrames)
					{
						// Check the clock.
						LONGLONG clockTime;
						MFTIME systemTime;
						if (FAILED(hr = this->_PresentationClock->GetCorrelatedTime(0, &clockTime, &systemTime)))	// Get the current time (before video processing).
							break;
						LONGLONG sampleTime;
						if (FAILED(hr = pSample->GetSampleTime(&sampleTime)))	// Get the display time of the sample.
							break;
						if (sampleTime < clockTime)
						{
							// If the sample is delayed with respect to the clock, drop the sample here.
							_OutputDebugString(_T("drop1.[sampleTime:%lld, clockTime:%lld]\n"), sampleTime / 10000, clockTime / 10000);
							break;
						}

						// Perform video processing on the sample with the presenter and obtain an output sample.
						if (FAILED(hr = this->_Presenter->ProcessFrame(this->_CurrentType, pSample, &this->_InterlaceMode, &bDeviceChanged, &bProcessAgain, &pOutSample)))
							break;

						// Notify the client if the device has changed.
						if (bDeviceChanged)
							if (FAILED(hr = this->QueueEvent(MEStreamSinkDeviceChanged, GUID_NULL, S_OK, NULL)))
								break;

						// If the input sample is not used, return it to the queue.
						if (bProcessAgain)
							if (FAILED(hr = this->_PreprocessingQueue->PutBack(pSample)))
								break;

						if (FAILED(hr = this->_PresentationClock->GetCorrelatedTime(0, &clockTime, &systemTime)))	// Get the current time again after video processing
							break;

						if (sampleTime < clockTime)
						{
							// If the sample is delayed relative to the clock, discard (drop) the sample here (part 2).
							_OutputDebugString(_T("drop2.[sampleTime:%lld, clockTime:%lld]\n"), sampleTime / 10000, clockTime / 10000);
							break;
						}

						if (NULL != pOutSample)
						{
							bool bPresentNow = (State_Started != this->_State);	// Immediately display in non-normal playback modes (scrubbing, redraws, etc.)
							hr = this->_Scheduler->ScheduleSample(pOutSample, bPresentNow);
							bProcessMoreSamples = FALSE;

							//_OutputDebugString(_T("scheduled.[sampleTime:%lld, clockTime:%lld]\n"), sampleTime / 10000, clockTime / 10000);
						}
					}
				}
			} while (FALSE);

			SafeRelease(pUnk);
			SafeRelease(pMarker);
			SafeRelease(pSample);
			SafeRelease(pOutSample);

			if (!bProcessMoreSamples)
				break;

		} // while loop

		SafeRelease(pUnk);

		return hr;
	}
	HRESULT StreamSink::RequestSamples()
	{
		//OutputDebugString(L"StreamSink::RequestSamples\n");

		HRESULT hr = S_OK;

		while (this->NeedMoreSamples())
		{
			// Is shutdown requested?
			if (FAILED(hr = this->CheckShutdown()))
				return hr;

			// Sends MEStreamSinkRequestSample to the client.
			this->_OutstandingSampleRequests++;
			if (FAILED(hr = this->QueueEvent(MEStreamSinkRequestSample, GUID_NULL, S_OK, NULL)))
				return hr;
		}

		return hr;
	}
	BOOL StreamSink::NeedMoreSamples()
	{
		const DWORD cSamplesInFlight = this->_PreprocessingQueue->GetCount() + this->_OutstandingSampleRequests;

		return (cSamplesInFlight < SAMPLE_QUEUE_HIWATER_THRESHOLD);
	}


	// AsyncOperation Implementation

	StreamSink::AsyncOperation::AsyncOperation(StreamOperation op)
	{
		this->m_nRefCount = 1;
		this->m_op = op;
	}
	StreamSink::AsyncOperation::~AsyncOperation()
	{
	}

	ULONG StreamSink::AsyncOperation::AddRef()
	{
		return InterlockedIncrement(&this->m_nRefCount);
	}
	HRESULT StreamSink::AsyncOperation::QueryInterface(REFIID iid, __RPC__deref_out _Result_nullonfailure_ void** ppv)
	{
		if (NULL == ppv)
			return E_POINTER;

		if (iid == IID_IUnknown)
		{
			*ppv = static_cast<IUnknown*>(this);
		}
		else
		{
			*ppv = NULL;
			return E_NOINTERFACE;
		}

		this->AddRef();

		return S_OK;
	}
	ULONG StreamSink::AsyncOperation::Release()
	{
		ULONG uCount = InterlockedDecrement(&this->m_nRefCount);

		if (uCount == 0)
			delete this;

		return uCount;
	}

}
