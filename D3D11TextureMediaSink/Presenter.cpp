#include "stdafx.h"

namespace D3D11TextureMediaSink
{
	Presenter::Presenter()
	{
		this->_ShutdownComplete = FALSE;
		this->_csPresenter = new CriticalSection();
		this->_SampleAllocator = new SampleAllocator();
	}
	Presenter::~Presenter()
	{
		this->_csPresenter = NULL;
	}

	void Presenter::SetD3D11(void* pDXGIDeviceManager, void* pD3D11Device)
	{
		this->_DXGIDeviceManager = (IMFDXGIDeviceManager*)pDXGIDeviceManager;
		this->_DXGIDeviceManager->AddRef();

		this->_D3D11Device = (ID3D11Device*)pD3D11Device;
		this->_D3D11Device->AddRef();

		this->_D3D11VideoDevice = NULL;
		this->_D3D11Device->QueryInterface(__uuidof(ID3D11VideoDevice), (void**)&this->_D3D11VideoDevice);

		// Attempt to initialize the sample allocator.
		this->InitializeSampleAllocator();
	}
	IMFDXGIDeviceManager* Presenter::GetDXGIDeviceManager()
	{
		this->_DXGIDeviceManager->AddRef();
		return this->_DXGIDeviceManager;
	}
	ID3D11Device* Presenter::GetD3D11Device()
	{
		this->_D3D11Device->AddRef();
		return this->_D3D11Device;
	}
	HRESULT Presenter::IsSupported(IMFMediaType* pMediaType, DXGI_FORMAT dxgiFormat)
	{
		HRESULT hr = S_OK;

		if (NULL == pMediaType)
			return E_POINTER;

		// Shut down?
		if (FAILED(hr = this->CheckShutdown()))
			return hr;

		UINT32 FrameRateNumerator = 30000, FrameRateDenominator = 1001;
		UINT32 ImageWidthPx, ImageHeightPx = 0;

		// Get the frame size (image width, height) from the media type to be scanned.
		if (FAILED(hr = ::MFGetAttributeSize(pMediaType, MF_MT_FRAME_SIZE, &ImageWidthPx, &ImageHeightPx)))
			return hr;

		// Get the frame rate (numerator, denominator) from the media type to be inspected.
		::MFGetAttributeRatio(pMediaType, MF_MT_FRAME_RATE, &FrameRateNumerator, &FrameRateDenominator);

		// Check if the format is supportable.
		D3D11_VIDEO_PROCESSOR_CONTENT_DESC ContentDesc;
		ZeroMemory(&ContentDesc, sizeof(ContentDesc));
		ContentDesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_INTERLACED_TOP_FIELD_FIRST;
		ContentDesc.InputWidth = (DWORD)ImageWidthPx;
		ContentDesc.InputHeight = (DWORD)ImageHeightPx;
		ContentDesc.OutputWidth = (DWORD)ImageWidthPx;
		ContentDesc.OutputHeight = (DWORD)ImageHeightPx;
		ContentDesc.InputFrameRate.Numerator = FrameRateNumerator;
		ContentDesc.InputFrameRate.Denominator = FrameRateDenominator;
		ContentDesc.OutputFrameRate.Numerator = FrameRateNumerator;
		ContentDesc.OutputFrameRate.Denominator = FrameRateDenominator;
		ContentDesc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

		// Get the ID3D11VideoProcessorEnumerator.
		ID3D11VideoProcessorEnumerator*	pVideoProcessorEnum = NULL;
		if (FAILED(hr = this->_D3D11VideoDevice->CreateVideoProcessorEnumerator(&ContentDesc, &pVideoProcessorEnum)))
			return hr;

		// Checks whether the format specified in the argument can be supported as input.
		UINT uiFlags;
		hr = pVideoProcessorEnum->CheckVideoProcessorFormat(dxgiFormat, &uiFlags);
		if (FAILED(hr) || 0 == (uiFlags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT))
			return MF_E_UNSUPPORTED_D3D_TYPE;

		return hr;
	}
	HRESULT Presenter::SetCurrentMediaType(IMFMediaType* pMediaType)
	{
		HRESULT hr = S_OK;

		// Shut down?
		if (FAILED(hr = this->CheckShutdown()))
			return hr;


		//
		// todo: If necessary, get the resolution, aspect ratio, etc. here.
		//



		// Get the frame size (image width, height) from the media type.
		if (FAILED(hr = ::MFGetAttributeSize(pMediaType, MF_MT_FRAME_SIZE, &this->_Width, &this->_Height)))
			return hr;

		// Get the format GUID from the media type.
		GUID subType;
		if (FAILED(hr = pMediaType->GetGUID(MF_MT_SUBTYPE, &subType)))
			return hr;

		// Attempt to initialize the sample allocator.
		this->InitializeSampleAllocator();

		return S_OK;
	}
	BOOL Presenter::IsReadyNextSample()
	{
		return this->_IsNextSampleReady;
	}

	void Presenter::Shutdown()
	{
		AutoLock(this->_csPresenter);

		if (!this->_ShutdownComplete)
		{
			//this._XVPControl = null;
			//this._XVP = null;

			SafeRelease(this->_DXGIDeviceManager);
			SafeRelease(this->_D3D11VideoDevice);
			SafeRelease(this->_D3D11Device);

			this->_ShutdownComplete = TRUE;
		}
	}
	HRESULT Presenter::Flush()
	{
		AutoLock lock(this->_csPresenter);

		HRESULT hr;

		// Shut down?
		if (FAILED(hr = this->CheckShutdown()))
			return hr;

		this->_IsNextSampleReady = TRUE;

		return S_OK;
	}
	HRESULT Presenter::ProcessFrame(_In_ IMFMediaType* pCurrentType, _In_ IMFSample* pSample, _Out_ UINT32* punInterlaceMode, _Out_ BOOL* pbDeviceChanged, _Out_ BOOL* pbProcessAgain, _Out_ IMFSample** ppOutputSample)
	{
		HRESULT hr;
		*pbDeviceChanged = FALSE;
		*pbProcessAgain = FALSE;

		// Shut down?
		if (FAILED(hr = this->CheckShutdown()))
			return hr;

		if (NULL == pCurrentType || NULL == pSample || NULL == punInterlaceMode || NULL == pbDeviceChanged || NULL == pbProcessAgain || NULL == ppOutputSample)
			return E_POINTER;


		IMFMediaBuffer* pBuffer = NULL;
		IMFDXGIBuffer* pMFDXGIBuffer = NULL;
		ID3D11Texture2D* pTexture2D = NULL;
		IMFSample* pRTSample = NULL;

		do
		{
			// Get IMFMediaBuffer from the input sample (IMFSample).
			DWORD cBuffers = 0;
			if (FAILED(hr = pSample->GetBufferCount(&cBuffers)))
				break;
			if (1 == cBuffers)
			{
				if (FAILED(hr = pSample->GetBufferByIndex(0, &pBuffer)))
					break;
			}
			else
			{
				if (FAILED(hr = pSample->ConvertToContiguousBuffer(&pBuffer)))
					break;
			}

			// Gets the interlaced mode of the input sample from the media type.
			MFVideoInterlaceMode unInterlaceMode = (MFVideoInterlaceMode) ::MFGetAttributeUINT32(pCurrentType, MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

			// If the interlaced mode of the media type is Mix mode, determine which input sample this time is.
			if (MFVideoInterlace_MixedInterlaceOrProgressive == unInterlaceMode)
			{
				if (FALSE == ::MFGetAttributeUINT32(pSample, MFSampleExtension_Interlaced, FALSE))
				{
					*punInterlaceMode = MFVideoInterlace_Progressive;	// Be progressive
				}
				else
				{
					if (::MFGetAttributeUINT32(pSample, MFSampleExtension_BottomFieldFirst, FALSE))
					{
						*punInterlaceMode = MFVideoInterlace_FieldInterleavedLowerFirst;	// Lower-priority interleaving
					}
					else
					{
						*punInterlaceMode = MFVideoInterlace_FieldInterleavedUpperFirst;	// Upper-priority interleaving
					}
				}
			}

			// Obtained IMFDXGIBuffer from IMFMediaBuffer.
			if (FAILED(hr = pBuffer->QueryInterface(__uuidof(IMFDXGIBuffer), (LPVOID*)&pMFDXGIBuffer)))
				break;

			// Obtained ID3D11Texture2D and dimensions from IMFDXGIBuffer.
			if (FAILED(hr = pMFDXGIBuffer->GetResource(__uuidof(ID3D11Texture2D), (LPVOID*)&pTexture2D)))
				break;
			UINT dwViewIndex = 0;
			if (FAILED(hr = pMFDXGIBuffer->GetSubresourceIndex(&dwViewIndex)))
				break;

			// ID3D11Texture2D is video-processed to obtain an output sample (IMFSample).
			if (FAILED(hr = this->ProcessFrameUsingD3D11(pTexture2D, dwViewIndex, *punInterlaceMode, ppOutputSample)))
				break;

			// Copy the time information from the input sample to the output sample.
			LONGLONG llv;
			if (SUCCEEDED(pSample->GetSampleTime(&llv)))
			{
				(*ppOutputSample)->SetSampleTime(llv);
				//TCHAR buf[1024];
				//wsprintf(buf, L"Presenter::ProcessFrame; Display time %d ms (%x)\n", (llv / 10000), *ppOutputSample);
				//OutputDebugString(buf);
			}
			if (SUCCEEDED(pSample->GetSampleDuration(&llv)))
			{
				(*ppOutputSample)->SetSampleDuration(llv);
			}
			DWORD flags;
			if (SUCCEEDED(pSample->GetSampleFlags(&flags)))
				(*ppOutputSample)->SetSampleFlags(flags);

		} while (FALSE);

		SafeRelease(pTexture2D);
		SafeRelease(pMFDXGIBuffer);
		SafeRelease(pBuffer);

		return S_OK;
	}
	HRESULT Presenter::ReleaseSample(IMFSample* pSample)
	{
		return this->_SampleAllocator->ReleaseSample(pSample);
	}

	// private

	HRESULT Presenter::CheckShutdown() const
	{
		return this->_ShutdownComplete ? MF_E_SHUTDOWN : S_OK;
	}
	HRESULT Presenter::InitializeSampleAllocator()
	{
		// Do you have the device, size, and format?
		if (NULL == this->_D3D11Device || this->_Width == 0 || this->_Height == 0)
			return MF_E_NOT_INITIALIZED;	// Not yet available

		this->_SampleAllocator->Shutdown();	// Shut it down just in case,
		return this->_SampleAllocator->Initialize(this->_D3D11Device, this->_Width, this->_Height);	// Initialization.
	}

	HRESULT Presenter::ProcessFrameUsingD3D11(ID3D11Texture2D* pTexture2D, UINT dwViewIndex, UINT32 unInterlaceMode, IMFSample** ppVideoOutFrame)
	{
		HRESULT hr = S_OK;
		ID3D11DeviceContext* pDeviceContext = NULL;
		ID3D11VideoContext* pVideoContext = NULL;
		IMFSample* pOutputSample = NULL;
		ID3D11Texture2D* pOutputTexture2D = NULL;
		ID3D11VideoProcessorOutputView* pOutputView = NULL;
		ID3D11VideoProcessorInputView* pInputView = NULL;

		do
		{
			// ID3D11DeviceContext get
			this->_D3D11Device->GetImmediateContext(&pDeviceContext);

			// ID3D11VideoContext get
			if (FAILED(hr = pDeviceContext->QueryInterface(__uuidof(ID3D11VideoContext), (void**)&pVideoContext)))
				break;

			// Get input texture information.
			D3D11_TEXTURE2D_DESC surfaceDesc;
			pTexture2D->GetDesc(&surfaceDesc);

			if (surfaceDesc.Width != this->_Width ||
				surfaceDesc.Height != this->_Height)
			{
				hr = MF_E_INVALID_STREAM_DATA;	// Ignore if it is different from the size specified by the media type.
				break;
			}

			// Create a video processor if you haven't already.
			if (NULL == this->_D3D11VideoProcessorEnum || NULL == this->_D3D11VideoProcessor)
			{
				SafeRelease(this->_D3D11VideoProcessor);		// Just in case
				SafeRelease(this->_D3D11VideoProcessorEnum);	//

				// Create a VideoProcessorEnumerator.
				D3D11_VIDEO_PROCESSOR_CONTENT_DESC ContentDesc;
				ZeroMemory(&ContentDesc, sizeof(ContentDesc));
				ContentDesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_INTERLACED_TOP_FIELD_FIRST;
				ContentDesc.InputWidth = surfaceDesc.Width;				// Input size and
				ContentDesc.InputHeight = surfaceDesc.Height;			//
				ContentDesc.OutputWidth = surfaceDesc.Width;			// output size are the same.
				ContentDesc.OutputHeight = surfaceDesc.Height;			//
				ContentDesc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;	// Purpose: Playback
				if (FAILED(hr = this->_D3D11VideoDevice->CreateVideoProcessorEnumerator(&ContentDesc, &this->_D3D11VideoProcessorEnum)))
					break;

				// If the VideoProcessor doesn't support DXGI_FORMAT_B8G8R8A8_UNORM output, we can't use it.
				UINT uiFlags;
				DXGI_FORMAT VP_Output_Format = DXGI_FORMAT_B8G8R8A8_UNORM;
				if (FAILED(hr = this->_D3D11VideoProcessorEnum->CheckVideoProcessorFormat(VP_Output_Format, &uiFlags)))
					return hr;
				if (0 == (uiFlags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT))
				{
					hr = MF_E_UNSUPPORTED_D3D_TYPE;	// Output cannot be supported.
					break;
				}

				// Find the index of the BOB Video Processor.

				DWORD indexOfBOB;
				if (FAILED(hr = this->FindBOBVideoProcessor(&indexOfBOB)))
					break;	// BOB not found...

				// Create the BOB Video Processor.
				if (FAILED(hr = this->_D3D11VideoDevice->CreateVideoProcessor(this->_D3D11VideoProcessorEnum, indexOfBOB, &this->_D3D11VideoProcessor)))
					break;
			}

			// Get the output sample.
			pOutputSample = NULL;
			if (FAILED(hr = this->_SampleAllocator->GetSample(&pOutputSample)))
				break;
			if (FAILED(hr = pOutputSample->SetUINT32(SAMPLE_STATE, SAMPLE_STATE_UPDATING)))
				break;

			// Get the output texture.
			IMFMediaBuffer* pBuffer = NULL;
			IMFDXGIBuffer* pMFDXGIBuffer = NULL;
			do
			{
				// Get the media buffer from the output sample.
				if (FAILED(hr = pOutputSample->GetBufferByIndex(0, &pBuffer)))
					break;

				// Get the IMFDXGIBuffer from the IMFMediaBuffer.
				if (FAILED(hr = pBuffer->QueryInterface(__uuidof(IMFDXGIBuffer), (LPVOID*)&pMFDXGIBuffer)))
					break;

				// Get the ID3D11Texture2D from the IMFDXGIBuffer.
				if (FAILED(hr = pMFDXGIBuffer->GetResource(__uuidof(ID3D11Texture2D), (LPVOID*)&pOutputTexture2D)))
					break;

			} while (FALSE);

			SafeRelease(pMFDXGIBuffer);
			SafeRelease(pBuffer);

			if (FAILED(hr))
				break;

			// Create an input view from the input texture.
			D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC InputLeftViewDesc;
			ZeroMemory(&InputLeftViewDesc, sizeof(InputLeftViewDesc));
			InputLeftViewDesc.FourCC = 0;
			InputLeftViewDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
			InputLeftViewDesc.Texture2D.MipSlice = 0;
			InputLeftViewDesc.Texture2D.ArraySlice = dwViewIndex;
			if (FAILED(hr = this->_D3D11VideoDevice->CreateVideoProcessorInputView(pTexture2D, this->_D3D11VideoProcessorEnum, &InputLeftViewDesc, &pInputView)))
				break;

			// Create an output view from the output texture.
			D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC OutputViewDesc;
			ZeroMemory(&OutputViewDesc, sizeof(OutputViewDesc));
			OutputViewDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
			OutputViewDesc.Texture2D.MipSlice = 0;
			OutputViewDesc.Texture2DArray.MipSlice = 0;
			OutputViewDesc.Texture2DArray.FirstArraySlice = 0;
			if (FAILED(hr = this->_D3D11VideoDevice->CreateVideoProcessorOutputView(pOutputTexture2D, this->_D3D11VideoProcessorEnum, &OutputViewDesc, &pOutputView)))
				break;

			// Set the parameters for the video context.
			{
				// Set the format for input stream 0.
				D3D11_VIDEO_FRAME_FORMAT FrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
				if ((MFVideoInterlace_FieldInterleavedUpperFirst == unInterlaceMode) ||
					(MFVideoInterlace_FieldSingleUpper == unInterlaceMode) ||
					(MFVideoInterlace_MixedInterlaceOrProgressive == unInterlaceMode))
				{
					FrameFormat = D3D11_VIDEO_FRAME_FORMAT_INTERLACED_TOP_FIELD_FIRST;
				}
				else if ((MFVideoInterlace_FieldInterleavedLowerFirst == unInterlaceMode) ||
					(MFVideoInterlace_FieldSingleLower == unInterlaceMode))
				{
					FrameFormat = D3D11_VIDEO_FRAME_FORMAT_INTERLACED_BOTTOM_FIELD_FIRST;
				}
				pVideoContext->VideoProcessorSetStreamFrameFormat(this->_D3D11VideoProcessor, 0, FrameFormat);

				// Set the output rate for input stream 0.
				pVideoContext->VideoProcessorSetStreamOutputRate(this->_D3D11VideoProcessor, 0, D3D11_VIDEO_PROCESSOR_OUTPUT_RATE_NORMAL, TRUE, NULL);

				// Set the source rectangle for input stream 0. This is a rectangle within the input surface specified in pixel coordinates relative to the input surface. (There is one for each input stream.)
				RECT sourceRect = { 0L, 0L, (LONG)surfaceDesc.Width, (LONG)surfaceDesc.Height };
				pVideoContext->VideoProcessorSetStreamSourceRect(this->_D3D11VideoProcessor, 0, TRUE, &sourceRect);

				// Set the destination rectangle for input stream 0. This is a rectangle within the output surface specified in pixel coordinates relative to the output surface. (There is one for each input stream.)
				RECT destRect = { 0L, 0L, (LONG)surfaceDesc.Width, (LONG)surfaceDesc.Height };
				pVideoContext->VideoProcessorSetStreamDestRect(this->_D3D11VideoProcessor, 0, TRUE, &destRect);

				// Set the output target rectangle. This is a rectangle within the output surface specified in pixel coordinates relative to the output surface. (There is only one.)
				RECT targetRect = { 0L, 0L, (LONG)surfaceDesc.Width, (LONG)surfaceDesc.Height };
				pVideoContext->VideoProcessorSetOutputTargetRect(this->_D3D11VideoProcessor, TRUE, &destRect);

				// Set the input color space for input stream 0.
				D3D11_VIDEO_PROCESSOR_COLOR_SPACE colorSpace = {};
				colorSpace.YCbCr_xvYCC = 1;
				pVideoContext->VideoProcessorSetStreamColorSpace(this->_D3D11VideoProcessor, 0, &colorSpace);

				// Set the output color space.
				pVideoContext->VideoProcessorSetOutputColorSpace(this->_D3D11VideoProcessor, &colorSpace);

				// Set the output background color to black.
				D3D11_VIDEO_COLOR backgroundColor = {};
				backgroundColor.RGBA.A = 1.0F;
				backgroundColor.RGBA.R = 1.0F * static_cast<float>(GetRValue(0)) / 255.0F;
				backgroundColor.RGBA.G = 1.0F * static_cast<float>(GetGValue(0)) / 255.0F;
				backgroundColor.RGBA.B = 1.0F * static_cast<float>(GetBValue(0)) / 255.0F;
				pVideoContext->VideoProcessorSetOutputBackgroundColor(this->_D3D11VideoProcessor, FALSE, &backgroundColor);
			}

			// Convert and process the input to the output.
			D3D11_VIDEO_PROCESSOR_STREAM StreamData;
			ZeroMemory(&StreamData, sizeof(StreamData));
			StreamData.Enable = TRUE;
			StreamData.OutputIndex = 0;
			StreamData.InputFrameOrField = 0;
			StreamData.PastFrames = 0;
			StreamData.FutureFrames = 0;
			StreamData.ppPastSurfaces = NULL;
			StreamData.ppFutureSurfaces = NULL;
			StreamData.pInputSurface = pInputView;
			StreamData.ppPastSurfacesRight = NULL;
			StreamData.ppFutureSurfacesRight = NULL;
			if (FAILED(hr = pVideoContext->VideoProcessorBlt(this->_D3D11VideoProcessor, pOutputView, 0, 1, &StreamData)))
				break;

			// If the output sample was created successfully, return it.
			if (NULL != ppVideoOutFrame)
			{
				*ppVideoOutFrame = pOutputSample;
				//(*ppVideoOutFrame)->AddRef();--> Do not AddRef/Release the sample owned by SampleAllocator
			}

		} while (FALSE);
		// Release resources
		SafeRelease(pInputView);
		SafeRelease(pOutputView);
		SafeRelease(pOutputTexture2D);
		//SafeRelease(pOutputSample); --> Do not AddRef/Release the sample owned by SampleAllocator
		SafeRelease(pVideoContext);
		SafeRelease(pDeviceContext);

		return hr;
	}
	HRESULT Presenter::FindBOBVideoProcessor(_Out_ DWORD* pIndex)
	{
		// ※ BOB does not require any reference frames, so it can be used for both progressive/interlaced video.

		HRESULT hr = S_OK;
		D3D11_VIDEO_PROCESSOR_CAPS caps = {};

		*pIndex = 0;

		if (FAILED(hr = this->_D3D11VideoProcessorEnum->GetVideoProcessorCaps(&caps)))
			return hr;

		for (DWORD i = 0; i < caps.RateConversionCapsCount; i++)
		{
			D3D11_VIDEO_PROCESSOR_RATE_CONVERSION_CAPS convCaps = {};
			if (FAILED(hr = this->_D3D11VideoProcessorEnum->GetVideoProcessorRateConversionCaps(i, &convCaps)))
				return hr;

			// If deinterlacing is supported, return the corresponding index.
			if (0 != (convCaps.ProcessorCaps & D3D11_VIDEO_PROCESSOR_PROCESSOR_CAPS_DEINTERLACE_BOB))
			{
				*pIndex = i;
				return S_OK;	// return
			}
		}

		return E_FAIL;	// not found
	}
}
