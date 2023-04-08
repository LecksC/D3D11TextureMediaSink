#pragma once

// {FD13125A-546C-4271-98DC-CA2058682AC7}
// Custom attribute (uint) for IMFSample
DEFINE_GUID(SAMPLE_STATE, 0xfd13125a, 0x546c, 0x4271, 0x98, 0xdc, 0xca, 0x20, 0x58, 0x68, 0x2a, 0xc7);
#define SAMPLE_STATE_READY		0	// Unused
#define SAMPLE_STATE_UPDATING	1	// Updating
#define SAMPLE_STATE_SCHEDULED	2	// Scheduled
#define SAMPLE_STATE_PRESENT	3	// Presenting

#define SAMPLE_MAX		5

namespace D3D11TextureMediaSink
{
	class SampleAllocator
	{
	public:
		SampleAllocator();
		~SampleAllocator();

		HRESULT Initialize(ID3D11Device* pD3DDevice, int width, int height);
		HRESULT Shutdown();
		HRESULT GetSample(IMFSample** ppSample);
		HRESULT ReleaseSample(IMFSample* pSample);

	private:
		BOOL _IsShutdown = TRUE;
		IMFSample* _SampleQueue[SAMPLE_MAX];
		CriticalSection _csSampleAllocator;
		HANDLE _FreeSampleAvailable;
		HRESULT CheckShutdown();
	};
}
