#pragma once

namespace D3D11TextureMediaSink
{
	// IMFStreamSink: A class that holds marker information for PlaceMarker.
	//
	class Marker : public IMarker
	{
	public:
		static HRESULT Create(
			MFSTREAMSINK_MARKER_TYPE eMarkerType,
			const PROPVARIANT* pvarMarkerValue,
			const PROPVARIANT* pvarContextValue,
			IMarker** ppMarker);

		// IUnknown Implementation
		STDMETHODIMP_(ULONG) AddRef();
		STDMETHODIMP_(ULONG) Release();
		STDMETHODIMP QueryInterface(REFIID iid, __RPC__deref_out _Result_nullonfailure_ void** ppv);

		// IMarker Implementation
		STDMETHODIMP GetType(MFSTREAMSINK_MARKER_TYPE* pType);
		STDMETHODIMP GetValue(PROPVARIANT* pvar);
		STDMETHODIMP GetContext(PROPVARIANT* pvar);

	protected:
		MFSTREAMSINK_MARKER_TYPE	mType;
		PROPVARIANT					mValue;
		PROPVARIANT					mContext;

	private:
		Marker(MFSTREAMSINK_MARKER_TYPE type);
		virtual ~Marker();

		long mReferenceCounter;
	};
}
