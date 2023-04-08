#pragma once

namespace D3D11TextureMediaSink
{
// IMarker:
// IMFStreamSink: A custom interface for handling placeMarker calls asynchronously.

//  The markers are composed of
// - Marker type (MFSTREAMSINK_MARKER_TYPE)
// - MARKER DATA (PROVARIANT)
// - CONTEXT (PROVARIANT)
//
//  This interface allows marker data to be stored inside an IUnknown object.
//  The object can be kept in the same queue as the queue holding the media type.
//  This is useful because samples and markers must be serialized.
//  In other words, you cannot be responsible for the marker until you have processed
//  all the samples that came before it.

//  Note that IMarker is not a standard Media Foundation interface.
	MIDL_INTERFACE("3AC82233-933C-43a9-AF3D-ADC94EABF406")
	IMarker : public IUnknown
	{
		virtual STDMETHODIMP GetType(MFSTREAMSINK_MARKER_TYPE* pType) = 0;
		virtual STDMETHODIMP GetValue(PROPVARIANT* pvar) = 0;
		virtual STDMETHODIMP GetContext(PROPVARIANT* pvar) = 0;
	};
}
