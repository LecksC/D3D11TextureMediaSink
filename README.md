### D3D11TextureMediaSink
D3D11TextureMediaSink is a custom MediaSink for MediaSession that outputs video in the form of Direct3D11 textures (ID3D11Texture2D). It was created based on Microsoft's DX11VideoRenderer sample.

When registering D3D11TextureMediaSink as a video renderer when constructing MediaSession, you can obtain an IMFSample COM object containing a frame image during video playback. The ID3D11Texture2D resource is included in the media buffer of this IMFSample. The texture format is fixed at DXGI_FORMAT_B8G8R8A8_UNorm. Up to five output textures that can be cached by the scheduler are available.

D3D11/DXVA2.0 is used for video processing after decoding (deinterlacing and color conversion). It does not support XVP (Media Foundation Transcode Video Processor), IMFActivate, or registration in the registry.

The solution includes the D3D11TextureMediaSink project (C++) that generates D3D11TextureMediaSink.lib/dll, the D3D11TextureMediaSinkDemo project (C++) that plays videos using D3D11TextureMediaSink.lib/dll, and the D3D11TextureMediaSinkDemoCSharp project (C#) that is a C# version of the sample for playing videos using D3D11TextureMediaSink.dll. To handle DirectX and Media Foundation in C#, SharpDX and MediaFoundation are obtained from NuGet at build time.

The development/operating requirements include Windows 10 October 2018 Update (1809), DirectX 11.0, Visual Studio Community 2017 Version 15.9.7, C++, Windows 10 SDK for October 2018 Update (10.0.17763.0), Visual Studio 2017 (v141) Toolset, C#, Visual C# 7.0, and .NET Framework 4.7.1.

The license is MIT License.

To use it:
# (1) Initialization
First, create ID3D11Device and IMFDXGIDeviceManager on the app side.

D3D11TextureMediaSink does not use the lock function of IMFDXGIDeviceManager and uses the passed ID3D11Device as it is. Therefore, ID3D11Device needs to be set to ID3D10Multithread::SetMultithreadProtected(TRUE).
Generate an IMFMediaSink object in the CreateD3D11TextureMediaSink function of D3D11TextureMediaSink.
Pass the IMFDXGIDeviceManager object and ID3D11Device object created by the app as arguments.


```
// Create a D3D11TextureMediaSink object and receive it as an IMFMediaSink interface.
HRESULT hr;
hr = CreateD3D11TextureMediaSink(
  IID_IMFMediaSink,
  (void**)&m_pMediaSink, // IMFMediaSink* m_pMediaSink
  m_pDXGIDeviceManager,  // IMFDXGIDeviceManager already created
  m_pD3DDevice);         // ID3D11Device already created
```
Also get the IMFAttributes necessary for receiving video frames from D3D11TextureMediaSink.
```
hr = m_pMediaSink->QueryInterface(
  IID_IMFAttributes, 
  (void**)&m_pMediaSinkAttributes); // IMFAttributes* m_pMediaSinkAttributes
```
# (2) Building MediaSession
Build the MediaSession.
When creating a partial topology, assign the first IMFStreamSink object that can be obtained from D3D11TextureMediaSink to the video output node (IMFTopologyNode).
```
// Get the IMFStreamSink for ID 0 (fixed value) from IMFMediaSink.
IMFStreamSink* pStreamSink;
hr = pMediaSink->GetStreamSinkById(0, &pStreamSink);

// Assign the IMFStreamSink to the output node.
hr = pOutputNode->SetObject(pStreamSink); // IMFTopologyNode* pOutputNode;
```
# (3) Acquiring and Drawing Video Frames
Start playing MediaSession.
Get the video frame (IMFSample) from the TMS_SAMPLE attribute of D3D11TextureMediaSink.
```
hr = m_pMediaSinkAttributes->GetUnknown(
  TMS_SAMPLE,         // GUID defined in D3D11TextureMediaSink.h
  IID_IMFSample,
  (void**)&pSample);  // IMFSample* pSample
```
Get the ID3D11Texture2D from the acquired IMFSample.
```
// Get the media buffer from the IMFSample.
IMFMediaBuffer* pMediaBuffer;
hr = pSample->GetBufferByIndex(0, &pMediaBuffer);

// Get the DXGI buffer from the media buffer.
IMFDXGIBuffer* pDXGIBuffer;
hr = pMediaBuffer->QueryInterface(IID_IMFDXGIBuffer, (void**)&pDXGIBuffer);

// Get Texture2D from the DXGI buffer.
ID3D11Texture2D* pTexture2D;
hr = pDXGIBuffer->GetResource(IID_ID3D11Texture2D, (void**)&pTexture2D);

// Get the subresource index of Texture2D.
UINT subIndex;
```
Draw the acquired ID3D11Texture2D resource in the desired way on the application side.

```
(omitted)
```
Release the IMFSample after the drawing is complete.
```
pTexture2D->Release();
pDXGIBuffer->Release();
pMediaBufer->Release();
pSample->Release();
hr = m_pMediaSinkAttributes->SetUnknown(TMS_SAMPLE, NULL);
```
# Note:
D3D11TextureMediaSink always swaps the IMFSample provided with the TMS_SAMPLE attribute to the latest one that should be displayed according to its internal scheduler.
When the app calls IMFAttributes::GetUnknown() for the TMS_SAMPLE attribute, D3D11TextureMediaSink temporarily holds (locks) this swapping process for the IMFSample.
When the app finishes drawing the IMFSample and no longer needs it, call IMFAttributes::SetUnknown() to declare its release. When this is called, D3D11TextureMediaSink resumes the swapping process for the latest IMFSample according to the schedule.

Further note:
The swapping process for IMFSample should not be held for a long time.
For example, if you set the ID3D11Texture2D resource acquired from the IMFSample to the pixel shader, you need to maintain it until the shader is executed. In such cases, it is recommended to copy the image to another resource for use.