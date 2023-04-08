#include "stdafx.h"

namespace D3D11TextureMediaSink
{
	SampleAllocator::SampleAllocator()
	{
	}
	SampleAllocator::~SampleAllocator()
	{
	}

	HRESULT SampleAllocator::Initialize(ID3D11Device* pD3DDevice, int width, int height)
	{
		HRESULT hr = S_OK;
		// Create an event object to signal when a sample becomes available.
		this->_FreeSampleAvailable = ::CreateEvent(NULL, FALSE, FALSE, NULL);	 // Note that usually the second argument is FALSE.

		for (int i = 0; i < SAMPLE_MAX; i++)
		{
			IMFSample* pSample = NULL;
			ID3D11Texture2D* pTexture = NULL;
			IMFMediaBuffer* pBuffer = NULL;

			do
			{
				// Create a new sample.
				if (FAILED(hr = ::MFCreateSample(&pSample)))
					break;

				// Create a texture resource.
				D3D11_TEXTURE2D_DESC desc;
				ZeroMemory(&desc, sizeof(desc));
				desc.ArraySize = 1;
				desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
				desc.CPUAccessFlags = 0;
				desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;	// Fixed format.
				desc.Width = width;		// Specified width.
				desc.Height = height;	// Specified height.
				desc.MipLevels = 1;
				desc.SampleDesc = { 1, 0 };
				desc.Usage = D3D11_USAGE_DEFAULT;
				if (FAILED(hr = pD3DDevice->CreateTexture2D(&desc, NULL, &pTexture)))
					break;

				// Create a media buffer from the texture.
				if (FAILED(hr = ::MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), pTexture, 0, FALSE, &pBuffer)))
					break;

				// Add the media buffer to the sample.
				if (FAILED(hr = pSample->AddBuffer(pBuffer)))
					break;

				// Initialize the sample state to READY.
				if (FAILED(hr = pSample->SetUINT32(SAMPLE_STATE, SAMPLE_STATE_READY)))
					break;

				// Add the completed sample to the queue.
				this->_SampleQueue[i] = pSample;
				this->_SampleQueue[i]->AddRef();

			} while (FALSE);

			SafeRelease(pBuffer);
			SafeRelease(pTexture);
			SafeRelease(pSample);

			if (FAILED(hr))
				return hr;
		}

		this->_IsShutdown = FALSE;

		return S_OK;
	}
	HRESULT SampleAllocator::Shutdown()
	{
		AutoLock lock(&this->_csSampleAllocator);

		if (this->_IsShutdown)
			return S_FALSE;

		this->_IsShutdown = TRUE;

		for (int i = 0; i < SAMPLE_MAX; i++)
			this->_SampleQueue[i]->Release();

		return S_OK;
	}
	HRESULT SampleAllocator::CheckShutdown()
	{
		return (this->_IsShutdown) ? MF_E_SHUTDOWN : S_OK;
	}

	HRESULT SampleAllocator::GetSample(IMFSample** ppSample)
	{
		HRESULT hr = S_OK;

		// Have we already shutdown?
		if (FAILED(hr = this->CheckShutdown()))
			return hr;

		if (NULL == ppSample)
			return E_POINTER;

		// Look for an available sample.
		while (TRUE)
		{
			int status[SAMPLE_MAX];

			{
				AutoLock lock(&this->_csSampleAllocator);

				for (int i = 0; i < SAMPLE_MAX; i++)
				{
					// If the value of the SAMPLE_STATE attribute is READY, the sample is available.

					UINT32 state;
					if (FAILED(hr = this->_SampleQueue[i]->GetUINT32(SAMPLE_STATE, &state)))
						return hr;

					status[i] = state;

					if (state == SAMPLE_STATE_READY)
					{
						*ppSample = this->_SampleQueue[i];	// The sample is available, so lend it out.
						(*ppSample)->AddRef();

						//TCHAR buf[1024];
						//wsprintf(buf, L"SampleAllocator::GetSample - [%d](%X)OK!\n", i, *ppSample);
						//OutputDebugString(buf);

						return S_OK;
					}
				}

			} // end of the lock scope

			// If there are no available samples, wait until one becomes available.
			if (::WaitForSingleObject(this->_FreeSampleAvailable, 5000) == WAIT_TIMEOUT)
				break;	 // If the wait times out, give up and return an error.
		}

		//OutputDebugString(L"SampleAllocator::GetSample - NoSample...\n");
		return MF_E_NOT_FOUND;
	}
	HRESULT SampleAllocator::ReleaseSample(IMFSample* pSample)
	{
		AutoLock lock(&this->_csSampleAllocator);

		HRESULT hr = S_OK;

		// Is the allocator shut down?
		if (FAILED(hr = this->CheckShutdown()))
			return hr;

		if (NULL == pSample)
			return E_POINTER;

		// Look for the sample that was lent out.
		for (int i = 0; i < SAMPLE_MAX; i++)
		{
			if (this->_SampleQueue[i] == pSample)
			{
				//pSample->Release();	--> Not releasing since we will reuse it.

				// Reset the SAMPLE_STATE attribute to READY.
				if (FAILED(hr = pSample->SetUINT32(SAMPLE_STATE, SAMPLE_STATE_READY)))
					return hr;

				//TCHAR buf[1024];
				//wsprintf(buf, L"SampleAllocator::ReleaseSample - [%d]OK!\n", i);
				//OutputDebugString(buf);

				// Notify that a sample is available.
				::SetEvent(this->_FreeSampleAvailable);

				return S_OK;
			}
		}

		//OutputDebugString(L"SampleAllocator::ReleaseSample - No Sample...\n");
		return MF_E_NOT_FOUND;	// The sample is not from our allocator.
	}
}
