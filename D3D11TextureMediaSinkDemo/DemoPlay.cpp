#include "stdafx.h"
#include "../D3D11TextureMediaSink/D3D11TextureMediaSink.h"
#include "DemoPlay.h"

DemoPlay::DemoPlay(HWND hWnd)
{
	HRESULT hr = S_OK;
	m_refCount = 0;
	m_bInitialized = FALSE;
	m_hrStatus = S_OK;
	m_bPresentNow = false;
	m_pWork = ::CreateThreadpoolWork(DemoPlay::PresentSwapChainWork, this, NULL);
	m_evReadyOrFailed = ::CreateEvent(NULL, TRUE, FALSE, NULL);

	// Setting up MediaFoundation
	::MFStartup(MF_VERSION);

	// Creating ID3D11Device and IDXGISwapChain
	D3D_FEATURE_LEVEL featureLevels[] =	{ D3D_FEATURE_LEVEL_11_0 };
	D3D_FEATURE_LEVEL feature_level;
	RECT clientRect;
	::GetClientRect(hWnd, &clientRect);
	DXGI_SWAP_CHAIN_DESC swapChainDesc;
	::ZeroMemory(&swapChainDesc, sizeof(swapChainDesc));
	swapChainDesc.BufferCount = 2;
	swapChainDesc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	swapChainDesc.BufferDesc.Width = clientRect.right - clientRect.left;
	swapChainDesc.BufferDesc.Height= clientRect.bottom - clientRect.top;
	swapChainDesc.BufferDesc.RefreshRate.Numerator = 60;
	swapChainDesc.BufferDesc.RefreshRate.Denominator = 1;
	swapChainDesc.BufferDesc.Scaling = DXGI_MODE_SCALING_STRETCHED;
	swapChainDesc.BufferDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
	swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	swapChainDesc.OutputWindow = hWnd;
	swapChainDesc.SampleDesc.Count = 1;
	swapChainDesc.SampleDesc.Quality = 0;
	swapChainDesc.Windowed = TRUE;
	hr = ::D3D11CreateDeviceAndSwapChain(
		NULL,
		D3D_DRIVER_TYPE_HARDWARE,
		NULL,
		D3D11_CREATE_DEVICE_BGRA_SUPPORT,
		featureLevels,
		1,
		D3D11_SDK_VERSION,
		&swapChainDesc,
		&m_pDXGISwapChain,
		&m_pD3DDevice,
		&feature_level,
		&m_pD3DDeviceContext);
	if (FAILED(hr))
		return;

	// Turn on multi-threaded mode. This is required when using DXVA.
	ID3D10Multithread* pD3D10Multithread;
	do
	{
		if (FAILED(hr = m_pD3DDevice->QueryInterface(IID_ID3D10Multithread, (void**)&pD3D10Multithread)))
			break;

		pD3D10Multithread->SetMultithreadProtected(TRUE);

	} while (FALSE);
	SafeRelease(pD3D10Multithread);
	if (FAILED(hr))
		return;

	// Create an instance of the IMFDXGIDeviceManager interface.
	if (FAILED(hr = ::MFCreateDXGIDeviceManager(&m_DXGIDeviceManagerID, &m_pDXGIDeviceManager)))
		return;
	m_pDXGIDeviceManager->ResetDevice(m_pD3DDevice, m_DXGIDeviceManagerID);

	// Obtaining the IDXGIOutput
	IDXGIDevice* pDXGIDevice;
	IDXGIAdapter* pDXGIAdapter;
	do
	{
		if (FAILED(hr = m_pD3DDevice->QueryInterface(IID_IDXGIDevice, (void**)&pDXGIDevice)))
			break;

		if (FAILED(hr = pDXGIDevice->GetAdapter(&pDXGIAdapter)))
			break;

		if (FAILED(hr = pDXGIAdapter->EnumOutputs(0, &m_pDXGIOutput)))
			break;
	
	} while (FALSE);
	SafeRelease(pDXGIAdapter);
	SafeRelease(pDXGIDevice);
	if (FAILED(hr))
		return;

	// ID for setting topology work queue. The content can be anything if it is of type IUnknown.
	IMFMediaType* pID = NULL;
	::MFCreateMediaType(&pID);
	this->ID_RegistarTopologyWorkQueueWithMMCSS = (IUnknown*)pID;

	// Initialization complete.
	m_bInitialized = TRUE;
}
DemoPlay::~DemoPlay()
{
}
void DemoPlay::Dispose()
{
	this->m_bInitialized = FALSE;

	// Stop video playback
	if (NULL != m_pMediaSession)
		m_pMediaSession->Stop();

	// Wait for the threadpool work to finish if it's currently being displayed
	::WaitForThreadpoolWorkCallbacks(m_pWork, TRUE);
	::CloseThreadpoolWork(m_pWork);
	
	// Release MediaFoundation resources
	SafeRelease(ID_RegistarTopologyWorkQueueWithMMCSS);
	SafeRelease(m_pMediaSinkAttributes);
	SafeRelease(m_pMediaSink);
	SafeRelease(m_pMediaSource);
	SafeRelease(m_pMediaSession);

	// Release DXGI, D3D11 resources
	SafeRelease(m_pDXGIDeviceManager);
	SafeRelease(m_pDXGIOutput);
	SafeRelease(m_pDXGISwapChain);
	SafeRelease(m_pD3DDeviceContext);
	SafeRelease(m_pD3DDevice);

	// Shutdown MediaFoundation
	::MFShutdown();
}
HRESULT DemoPlay::Play(LPCTSTR szFile)
{
	HRESULT hr = S_OK;

	::ResetEvent(m_evReadyOrFailed);

	// Create MediaSession.
	if (FAILED(hr = this->CreateMediaSession(&m_pMediaSession)))
		return hr;

	// Create a MediaSource from a file.
	if (FAILED(hr = this->CreateMediaSourceFromFile(szFile, &m_pMediaSource)))
		return hr;

	IMFTopology* pTopology = NULL;
	do
	{
		// Create a partial topology.
		if (FAILED(hr = this->CreateTopology(&pTopology)))
			break;

		// Register the partial topology with the MediaSession.
		if (FAILED(hr = m_pMediaSession->SetTopology(0, pTopology)))
			break;

	} while (FALSE);

	SafeRelease(pTopology);

	if (FAILED(hr))
		return hr;

	// Wait for the MESessionTopologyStatus event to be fired when the MediaSession has successfully (or unsuccessfully) created the full topology.
	::WaitForSingleObject(m_evReadyOrFailed, 5000);

	if (FAILED(m_hrStatus))
		return m_hrStatus;	// Creation failed

	// Start playback of the MediaSession.
	PROPVARIANT prop;
	::PropVariantInit(&prop);
	m_pMediaSession->Start(NULL, &prop);
	
	return S_OK;
}
void DemoPlay::UpdateAndPresent()
{
	if (!m_bPresentNow )	// m_bPresentNow is an atomic_bool
	{
		// Draw
		this->Draw();

		// Display the SwapChain
		m_bPresentNow = true;
		::SubmitThreadpoolWork(m_pWork);
	}
	else
	{
		// Progress processing
		this->Update();
	}
}
void DemoPlay::Update()
{
	// todo: processing to be done other than drawing
}
void DemoPlay::Draw()
{
	HRESULT hr = S_OK;

	IMFSample* pSample = NULL;
	IMFMediaBuffer* pMediaBuffer = NULL;
	IMFDXGIBuffer* pDXGIBuffer = NULL;
	ID3D11Texture2D* pTexture2D = NULL;
	ID3D11Texture2D* pBackBufferTexture2D = NULL;

	do
	{
		// Get the IMFSample that should currently be displayed, which is set to the TMS_SAMPLE attribute of TextureMediaSink.
		// If the attribute is not set or the operation fails, break out of the loop.
		if (FAILED(hr = m_pMediaSinkAttributes->GetUnknown(TMS_SAMPLE, IID_IMFSample, (void**)&pSample)))
			break;
		if (NULL == pSample)
			break;	// Not set

		// Retrieve the media buffer from the IMFSample.
		if (FAILED(hr = pSample->GetBufferByIndex(0, &pMediaBuffer)))
			break;

		// Retrieve the DXGI buffer from the media buffer.
		if (FAILED(hr = pMediaBuffer->QueryInterface(IID_IMFDXGIBuffer, (void**)&pDXGIBuffer)))
			break;

		// Retrieve the source Texture2D from the DXGI buffer.
		if (FAILED(hr = pDXGIBuffer->GetResource(IID_ID3D11Texture2D, (void**)&pTexture2D)))
			break;

		// Retrieve the sub-resource index of the source Texture2D.
		UINT subIndex;
		if (FAILED(hr = pDXGIBuffer->GetSubresourceIndex(&subIndex)))
			break;

		//
		// We have now obtained the ID3D11Texture2D from the IMFSample.
		// For this demo, we will simply draw a rectangle to the SwapChain.
		//

		// Retrieve the destination Texture2D from the SwapChain.
		if (FAILED(hr = m_pDXGISwapChain->GetBuffer(0, IID_ID3D11Texture2D, (void**)&pBackBufferTexture2D)))
			break;

		// Perform the transfer.
		m_pD3DDeviceContext->CopySubresourceRegion(pBackBufferTexture2D, 0, 0, 0, 0, pTexture2D, subIndex, NULL);

	} while (FALSE);

	// The IMFSample obtained with GetUnknown for TMS_SAMPLE is locked to prevent it from being updated by TextureMediaSink.
	// To release this lock, set anything(it doesn't matter what) to the TMS_SAMPLE attribute with SetUnknown.
	// This will make the IMFSample updatable from TextureMediaSink.
	m_pMediaSinkAttributes->SetUnknown(TMS_SAMPLE, NULL);

	SafeRelease(pBackBufferTexture2D);
	SafeRelease(pTexture2D);
	SafeRelease(pDXGIBuffer);
	SafeRelease(pMediaBuffer);
	SafeRelease(pSample);
}

HRESULT DemoPlay::CreateMediaSession(IMFMediaSession** ppMediaSession)
{
	HRESULT hr = S_OK;

	IMFMediaSession* pMediaSession = nullptr;
	do
	{
		// Create a MediaSession.
		if (FAILED(hr = ::MFCreateMediaSession(NULL, &pMediaSession)))
			break;

		// Begin an asynchronous request for events from the Media Session.
		if (FAILED(hr = pMediaSession->BeginGetEvent(this, nullptr)))
			break;

		// Return the result.
		(*ppMediaSession) = pMediaSession;
		(*ppMediaSession)->AddRef();
		hr = S_OK;

	} while (FALSE);

	SafeRelease(pMediaSession);

	return hr;
}
HRESULT DemoPlay::CreateMediaSourceFromFile(LPCTSTR szFile, IMFMediaSource** ppMediaSource)
{
	HRESULT hr = S_OK;

	IMFSourceResolver* pResolver = nullptr;
	IMFMediaSource* pMediaSource = nullptr;
	do
	{
		// Create a source resolver.
		if (FAILED(hr = ::MFCreateSourceResolver(&pResolver)))
			break;

		// Use the source resolver to create a media source from a URL.
		MF_OBJECT_TYPE type;
		if (FAILED(hr = pResolver->CreateObjectFromURL(szFile, MF_RESOLUTION_MEDIASOURCE, NULL, &type, (IUnknown**)&pMediaSource)))
			break;

		// Return the result.
		(*ppMediaSource) = pMediaSource;
		(*ppMediaSource)->AddRef();
		hr = S_OK;

	} while (FALSE);

	SafeRelease(pMediaSource);
	SafeRelease(pResolver);

	return hr;
}
HRESULT DemoPlay::CreateTopology(IMFTopology** ppTopology)
{
	HRESULT hr = S_OK;

	IMFTopology* pTopology = NULL;
	IMFPresentationDescriptor* pPresentationDesc = NULL;
	do
	{
		// Create a new topology.
		if (FAILED(hr = ::MFCreateTopology(&pTopology)))
			break;

		// Create a presentation descriptor for the media source.
		if(FAILED(hr = m_pMediaSource->CreatePresentationDescriptor(&pPresentationDesc)))
			break;

		// Get the number of stream descriptors for the media source from the presentation descriptor.
		DWORD dwDescCount;
		if (FAILED(hr = pPresentationDesc->GetStreamDescriptorCount(&dwDescCount)))
			break;

		// For each stream in the media source, create a topology node and add it to the topology.
		for (DWORD i = 0; i < dwDescCount; i++)
		{
			if (FAILED(hr = this->AddTopologyNodes(pTopology, pPresentationDesc, i)))
				break;
		}
		if (FAILED(hr))
			break;

		// Topology creation is complete.
		(*ppTopology) = pTopology;
		(*ppTopology)->AddRef();
		hr = S_OK;

	} while (FALSE);

	SafeRelease(pPresentationDesc);
	SafeRelease(pTopology);

	return hr;
}
HRESULT DemoPlay::AddTopologyNodes(IMFTopology* pTopology, IMFPresentationDescriptor* pPresentationDesc, DWORD index)
{
	HRESULT hr = S_OK;

	BOOL bSelected;
	IMFStreamDescriptor* pStreamDesc = NULL;
	IMFTopologyNode* pSourceNode = NULL;
	IMFTopologyNode* pOutputNode = NULL;
	do
	{
		// Get the stream descriptor for the specified stream index.
		if (FAILED(hr = pPresentationDesc->GetStreamDescriptorByIndex(index, &bSelected, &pStreamDesc)))
			break;

		if (bSelected)
		{
			// (A) If the stream is selected, add it to the topology.
			if (FAILED(hr = this->CreateSourceNode(pPresentationDesc, pStreamDesc, &pSourceNode)))
				break;

			GUID majorType;
			if (FAILED(hr = this->CreateOutputNode(pStreamDesc, &pOutputNode, &majorType)))
				break;

			if (majorType == MFMediaType_Audio)
			{
				pSourceNode->SetString(MF_TOPONODE_WORKQUEUE_MMCSS_CLASS, _T("Audio"));
				pSourceNode->SetUINT32(MF_TOPONODE_WORKQUEUE_ID, 1);
			}
			if (majorType == MFMediaType_Video)
			{
				pSourceNode->SetString(MF_TOPONODE_WORKQUEUE_MMCSS_CLASS, _T("Playback"));
				pSourceNode->SetUINT32(MF_TOPONODE_WORKQUEUE_ID, 2);
			}

			if (NULL != pSourceNode && NULL != pOutputNode)
			{
				pTopology->AddNode(pSourceNode);
				pTopology->AddNode(pOutputNode);

				pSourceNode->ConnectOutput(0, pOutputNode, 0);
			}
		}
		else
		{
			// (B) If the stream is not selected, do nothing.
		}

	} while (FALSE);

	SafeRelease(pOutputNode);
	SafeRelease(pSourceNode);
	SafeRelease(pStreamDesc);

	return hr;
}
HRESULT DemoPlay::CreateSourceNode(IMFPresentationDescriptor* pPresentationDesc, IMFStreamDescriptor* pStreamDesc, IMFTopologyNode** ppSourceNode)
{
	HRESULT hr = S_OK;

	IMFTopologyNode* pSourceNode = NULL;
	do
	{
		// Create a source node.
		if (FAILED(hr = ::MFCreateTopologyNode(MF_TOPOLOGY_SOURCESTREAM_NODE, &pSourceNode)))
			break;

		// Set three attributes to the source node.
		if (FAILED(hr = pSourceNode->SetUnknown(MF_TOPONODE_SOURCE, m_pMediaSource)))
			break;
		if (FAILED(hr = pSourceNode->SetUnknown(MF_TOPONODE_PRESENTATION_DESCRIPTOR, pPresentationDesc)))
			break;
		if (FAILED(hr = pSourceNode->SetUnknown(MF_TOPONODE_STREAM_DESCRIPTOR, pStreamDesc)))
			break;
	
		// Creation completed.
		(*ppSourceNode) = pSourceNode;
		(*ppSourceNode)->AddRef();
		hr = S_OK;

	} while (FALSE);

	SafeRelease(pSourceNode);

	return hr;
}
HRESULT DemoPlay::CreateOutputNode(IMFStreamDescriptor* pStreamDesc, IMFTopologyNode** ppOutputNode, GUID* pMajorType)
{
	HRESULT hr = S_OK;

	IMFTopologyNode* pOutputNode = NULL;
	IMFMediaTypeHandler* pMediaTypeHandler = NULL;
	do
	{
		// Create an output node.
		if (FAILED(hr = ::MFCreateTopologyNode(MF_TOPOLOGY_OUTPUT_NODE, &pOutputNode)))
			break;

		// Get the media type handler.
		if (FAILED(hr = pStreamDesc->GetMediaTypeHandler(&pMediaTypeHandler)))
			break;

		// Get the major media type.
		GUID majorType;
		if (FAILED(hr = pMediaTypeHandler->GetMajorType(&majorType)))
			break;

		if (majorType == MFMediaType_Video)
		{
			// (A) Video renderer

			if (NULL == m_pMediaSink)
			{
				// Create a TextureMediaSink.
				if (FAILED(hr = ::CreateD3D11TextureMediaSink(IID_IMFMediaSink, (void**)&m_pMediaSink, m_pDXGIDeviceManager, m_pD3DDevice)))
					break;

				// Get IMFAttributes to receive IMFSample.
				if (FAILED(hr = m_pMediaSink->QueryInterface(IID_IMFAttributes, (void**)&m_pMediaSinkAttributes)))
					break;
			}

			IMFStreamSink* pStreamSink = NULL;
			do
			{
				// Get stream sink #0.
				if (FAILED(hr = m_pMediaSink->GetStreamSinkById(0, &pStreamSink)))
					break;

				// Register the stream sink with the output node.
				if (FAILED(hr = pOutputNode->SetObject(pStreamSink)))
					break;

			} while (FALSE);

			SafeRelease(pStreamDesc);
		}
		else if (majorType == MFMediaType_Audio)
		{
			// (B) Audio renderer

			IMFActivate* pActivate = NULL;
			do
			{
				// Generate an activate of the default audio renderer.
				if (FAILED(hr = ::MFCreateAudioRendererActivate(&pActivate)))
					break;

				// Register the activate with the output node.
				if (FAILED(hr = pOutputNode->SetObject(pActivate)))
					break;

			} while (FALSE);

			SafeRelease(pActivate);
		}

		// Finished creating.
		(*ppOutputNode) = pOutputNode;
		(*ppOutputNode)->AddRef();
		hr = S_OK;

	} while (FALSE);

	SafeRelease(pMediaTypeHandler);
	SafeRelease(pOutputNode);

	return hr;
}

// IUnknown Implementation
ULONG	DemoPlay::AddRef()
{
	return InterlockedIncrement(&this->m_refCount);
}
HRESULT DemoPlay::QueryInterface(REFIID iid, __RPC__deref_out _Result_nullonfailure_ void** ppv)
{
	if (NULL == ppv)
		return E_POINTER;

	if (iid == IID_IUnknown)
	{
		*ppv = static_cast<IUnknown*>(static_cast<IMFAsyncCallback*>(this));
	}
	else if (iid == __uuidof(IMFAsyncCallback))
	{
		*ppv = static_cast<IMFAsyncCallback*>(this);
	}
	else
	{
		*ppv = NULL;
		return E_NOINTERFACE;
	}

	this->AddRef();

	return S_OK;
}
ULONG	DemoPlay::Release()
{
	ULONG uCount = InterlockedDecrement(&this->m_refCount);

	if (uCount == 0)
		delete this;

	return uCount;
}

// IMFAsyncCallback implementation
STDMETHODIMP DemoPlay::GetParameters(__RPC__out DWORD *pdwFlags, __RPC__out DWORD *pdwQueue)
{
	// Implementation of this method is optional.
	return E_NOTIMPL;
}
STDMETHODIMP DemoPlay::Invoke(__RPC__in_opt IMFAsyncResult *pAsyncResult)
{
	if (!m_bInitialized)
		return MF_E_SHUTDOWN;

	HRESULT hr = S_OK;

	IUnknown* pUnk;
	if (SUCCEEDED(hr = pAsyncResult->GetState(&pUnk)))
	{
		// (A) State exists (not E_POINTER error).

		if (this->ID_RegistarTopologyWorkQueueWithMMCSS == pUnk)
		{
			this->OnEndRegistarTopologyWorkQueueWithMMCSS(pAsyncResult);
			return S_OK;
		}
		else
		{
			return E_INVALIDARG;
		}
	}
	else
	{
		// (B) State does not exist.

		IMFMediaEvent* pMediaEvent = NULL;
		MediaEventType eventType;
		do
		{
			// Receive events from the MediaSession.
			if (FAILED(hr = m_pMediaSession->EndGetEvent(pAsyncResult, &pMediaEvent)))
				break;
			if (FAILED(hr = pMediaEvent->GetType(&eventType)))
				break;
			if (FAILED(hr = pMediaEvent->GetStatus(&m_hrStatus)))
				break;

			// If the result is a failure, exit.
			if (FAILED(m_hrStatus))
			{
				::SetEvent(m_evReadyOrFailed);
				return m_hrStatus;
			}

			// Branch by event type.
			switch (eventType)
			{
			case MESessionTopologyStatus:

				// Get status.
				UINT32 topoStatus;
				if (FAILED(hr = pMediaEvent->GetUINT32(MF_EVENT_TOPOLOGY_STATUS, &topoStatus)))
					break;
				switch (topoStatus)
				{
				case MF_TOPOSTATUS_READY:
					this->OnTopologyReady(pMediaEvent);
					break;
				}
				break;

			case MESessionStarted:
				this->OnSessionStarted(pMediaEvent);
				break;

			case MESessionPaused:
				this->OnSessionPaused(pMediaEvent);
				break;

			case MESessionStopped:
				this->OnSessionStopped(pMediaEvent);
				break;

			case MESessionClosed:
				this->OnSessionClosed(pMediaEvent);
				break;

			case MEEndOfPresentation:
				this->OnPresentationEnded(pMediaEvent);
				break;
			}

		} while (FALSE);

		SafeRelease(pMediaEvent);

		// Wait for the next MediaSession event.
		if (eventType != MESessionClosed)
			hr = m_pMediaSession->BeginGetEvent(this, NULL);

		return hr;
	}
}

void DemoPlay::OnTopologyReady(IMFMediaEvent* pMediaEvent)
{
	HRESULT hr;

	// Begin assigning the class to MMCSS for the topology work queue (asynchronous processing).

	IMFGetService* pGetService = NULL;
	IMFWorkQueueServices* pWorkQueueServices = NULL;
	do
	{
		if (FAILED(hr = this->m_pMediaSession->QueryInterface(IID_IMFGetService, (void**)&pGetService)))
			break;

		if (FAILED(hr = pGetService->GetService(MF_WORKQUEUE_SERVICES, IID_IMFWorkQueueServices, (void**)&pWorkQueueServices)))
			break;

		if (FAILED(hr = pWorkQueueServices->BeginRegisterTopologyWorkQueuesWithMMCSS(this, this->ID_RegistarTopologyWorkQueueWithMMCSS)))
			break;

	} while (FALSE);

	SafeRelease(pWorkQueueServices);
	SafeRelease(pGetService);

	if (FAILED(hr))
	{
		this->m_hrStatus = hr;
		::SetEvent(this->m_evReadyOrFailed);
	}
}
void DemoPlay::OnSessionStarted(IMFMediaEvent* pMediaEvent)
{
}
void DemoPlay::OnSessionPaused(IMFMediaEvent* pMediaEvent)
{
}
void DemoPlay::OnSessionStopped(IMFMediaEvent* pMediaEvent)
{
}
void DemoPlay::OnSessionClosed(IMFMediaEvent* pMediaEvent)
{
}
void DemoPlay::OnPresentationEnded(IMFMediaEvent* pMediaEvent)
{
}
void DemoPlay::OnEndRegistarTopologyWorkQueueWithMMCSS(IMFAsyncResult* pAsyncResult)
{
	HRESULT hr;

	// Complete the class assignment of the topology work queue to MMCSS (asynchronous processing).

	IMFGetService* pGetService = NULL;
	IMFWorkQueueServices* pWorkQueueServices = NULL;
	do
	{
		if (FAILED(hr = this->m_pMediaSession->QueryInterface(IID_IMFGetService, (void**)&pGetService)))
			break;

		if (FAILED(hr = pGetService->GetService(MF_WORKQUEUE_SERVICES, IID_IMFWorkQueueServices, (void**)&pWorkQueueServices)))
			break;

		if (FAILED(hr = pWorkQueueServices->EndRegisterTopologyWorkQueuesWithMMCSS(pAsyncResult)))
			break;

	} while (FALSE);

	SafeRelease(pWorkQueueServices);
	SafeRelease(pGetService);


	if (FAILED(hr))
		this->m_hrStatus = hr;
	else
		this->m_hrStatus = S_OK;

	::SetEvent(this->m_evReadyOrFailed);	// Trigger an event.
}

// Task to present SwapChain
void CALLBACK DemoPlay::PresentSwapChainWork(PTP_CALLBACK_INSTANCE pInstance, LPVOID pvParam, PTP_WORK pWork)
{
	auto pDemoPlay = (DemoPlay*)pvParam;

	pDemoPlay->m_pDXGIOutput->WaitForVBlank();
	pDemoPlay->m_pDXGISwapChain->Present(1, 0);

	pDemoPlay->m_bPresentNow = false;
}

