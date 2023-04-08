#include "stdafx.h"

namespace D3D11TextureMediaSink
{
	Marker::Marker(MFSTREAMSINK_MARKER_TYPE type)
	{
		this->mReferenceCounter = 1;
		this->mType = type;
		::PropVariantInit(&this->mValue);
		::PropVariantInit(&this->mContext);
	}
	Marker::~Marker()
	{
		::PropVariantClear(&this->mValue);
		::PropVariantClear(&this->mContext);
	}

	// static

	HRESULT Marker::Create(
		MFSTREAMSINK_MARKER_TYPE eMarkerType,
		const PROPVARIANT* pvarMarkerValue, // Can be NULL.
		const PROPVARIANT* pvarContextValue, // Can be NULL.
		IMarker** ppMarker // [out] A pointer to receive the IMarker you created.
	)
	{
		if (NULL == ppMarker)
			return E_POINTER;

		HRESULT hr = S_OK;

		Marker* pMarker = new Marker(eMarkerType);
		do
		{
			// Creates a new Marker with the specified type.
			if (NULL == pMarker)
			{
				hr = E_OUTOFMEMORY;
				break;
			}

			// Copy the specified value to the Marker value.
			if (NULL != pvarMarkerValue)
				if (FAILED(hr = ::PropVariantCopy(&pMarker->mValue, pvarMarkerValue)))
					break;

			// Copy the specified context to the CMarker context.
			if (pvarContextValue)
				if (FAILED(hr = ::PropVariantCopy(&pMarker->mContext, pvarContextValue)))
					break;

			// Update the reference counter for the return value and complete.
			*ppMarker = pMarker;
			(*ppMarker)->AddRef();

		} while (FALSE);

		SafeRelease(pMarker);

		return hr;
	}

	// IUnknown implementation
	ULONG Marker::AddRef()
	{
		return InterlockedIncrement(&this->mReferenceCounter);
	}
	ULONG Marker::Release()
	{
		// To make it thread-safe, get the value transferred to a temporary variable and return it.
		ULONG uCount = InterlockedDecrement(&this->mReferenceCounter);

		if (uCount == 0)
			delete this;

		return uCount;
	}
	HRESULT Marker::QueryInterface(REFIID iid, __RPC__deref_out _Result_nullonfailure_ void** ppv)
	{
		if (!ppv)
			return E_POINTER;

		if (iid == IID_IUnknown)
		{
			*ppv = static_cast<IUnknown*>(this);
		}
		else if (iid == __uuidof(IMarker))
		{
			*ppv = static_cast<IMarker*>(this);
		}
		else
		{
			*ppv = NULL;
			return E_NOINTERFACE;
		}

		this->AddRef();

		return S_OK;
	}

	// IMarker implementation
	HRESULT Marker::GetType(MFSTREAMSINK_MARKER_TYPE* pType)
	{
		if (pType == NULL)
			return E_POINTER;

		*pType = this->mType;

		return S_OK;
	}
	HRESULT Marker::GetValue(PROPVARIANT* pvar)
	{
		if (pvar == NULL)
			return E_POINTER;

		return ::PropVariantCopy(pvar, &(this->mValue));

	}
	HRESULT Marker::GetContext(PROPVARIANT* pvar)
	{
		if (pvar == NULL)
			return E_POINTER;

		return ::PropVariantCopy(pvar, &(this->mContext));
	}
}
