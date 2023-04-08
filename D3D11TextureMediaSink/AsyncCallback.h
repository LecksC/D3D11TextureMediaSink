#pragma once

namespace D3D11TextureMediaSink
{
	// AsyncCallback [template class]
	//
	// A class that implements IMFAsyncCallback. Routes the IMFAsyncCallback::Invoke call to the specified method of the specified instance.
	// Used when you want to implement multiple IMFAsyncCallback in a single class.
	//
	// Example usage:
	//
	// class CTest : public IUnknown
	// {
	// public:
	// CTest() :
	// m_cb1(this, &CTest::OnInvoke1), // m_cb1::Invoke calls this->OnInvoke1().
	// m_cb2(this, &CTest::OnInvoke2) // m_cb2::Invoke calls this->OnInvoke2().
	// { }
	// STDMETHODIMP_(ULONG) AddRef();
	// STDMETHODIMP_(ULONG) Release();
	// STDMETHODIMP QueryInterface(REFIID iid, __RPC__deref_out Result_nullonfailure void** ppv);
	//
	// private:
	// AsyncCallback<CTest> m_cb1;
	// AsyncCallback<CTest> m_cb2;
	// void OnInvoke1(IMFAsyncResult* pAsyncResult);
	// void OnInvoke2(IMFAsyncResult* pAsyncResult);
	// }
	//
	template<class T>	// T: The type of the parent object. The parent object is a COM object and provides a member of type InvokeFn.
	class AsyncCallback : public IMFAsyncCallback
	{
	public:
		typedef HRESULT(T::*InvokeFn)(IMFAsyncResult* pAsyncResult);

		AsyncCallback(T* pParent, InvokeFn fn)
		{
			this->m_pParent = pParent;
			this->m_pInvokeFn = fn;
		}
		~AsyncCallback()
		{
			this->m_pInvokeFn = nullptr;
			this->m_pParent = nullptr;
		}

		// IUnknown implementation.
		STDMETHODIMP_(ULONG) AddRef()
		{
			return this->m_pParent->AddRef();	// êDelegate to the parent object.
		}
		STDMETHODIMP QueryInterface(REFIID iid, __RPC__deref_out _Result_nullonfailure_ void** ppv)
		{
			if (!ppv)
				return E_POINTER;

			if (iid == __uuidof(IUnknown))
			{
				*ppv = static_cast<IUnknown*>(static_cast<IMFAsyncCallback*>(this));
			}
			else if (iid == __uuidof(IMFAsyncCallback))
			{
				*ppv = static_cast<IMFAsyncCallback*>(this);
			}
			else
			{
				*ppv = nullptr;
				return E_NOINTERFACE;
			}

			this->AddRef();

			return S_OK;
		}
		STDMETHODIMP_(ULONG) Release()
		{
			return this->m_pParent->Release();	// êDelegate to the parent object.
		}

		// IMFAsyncCallback Implementation
		STDMETHODIMP GetParameters(__RPC__out DWORD* pdwFlags, __RPC__out DWORD* pdwQueue)
		{
			// This method implementation is optional.
			return E_NOTIMPL;
		}
		STDMETHODIMP Invoke(__RPC__in_opt IMFAsyncResult* pAsyncResult)
		{
			// Call the callback.
			return (this->m_pParent->*m_pInvokeFn)(pAsyncResult);
		}

	private:
		T* m_pParent;
		InvokeFn m_pInvokeFn;
	};
}
