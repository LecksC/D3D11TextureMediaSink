#pragma once

namespace D3D11TextureMediaSink
{
	// Thread-safe queue with non-COM objects as elements.
	// 
	template <class T>
	class ThreadSafePtrQueue
	{
	public:
		ThreadSafePtrQueue()
		{
			::InitializeCriticalSection(&m_lock);
		}
		virtual ~ThreadSafePtrQueue()
		{
			::DeleteCriticalSection(&m_lock);
		}

		HRESULT Queue(T* p)
		{
			::EnterCriticalSection(&m_lock);

			HRESULT hr = m_list.InsertBack(p);

			::LeaveCriticalSection(&m_lock);

			return hr;
		}
		HRESULT Dequeue(T** pp)
		{
			::EnterCriticalSection(&m_lock);

			HRESULT hr = S_OK;
			if (m_list.IsEmpty())
			{
				*pp = NULL;
				hr = S_FALSE;
			}
			else
			{
				hr = m_list.RemoveFront(pp);
			}

			::LeaveCriticalSection(&m_lock);

			return hr;
		}
		HRESULT PutBack(T* p)
		{
			::EnterCriticalSection(&m_lock);

			HRESULT hr = m_list.InsertFront(p);

			::LeaveCriticalSection(&m_lock);

			return hr;
		}
		DWORD GetCount(void)
		{
			::EnterCriticalSection(&m_lock);

			DWORD nCount = m_list.GetCount();

			::LeaveCriticalSection(&m_lock);
			return nCount;
		}

		void Clear(void)
		{
			::EnterCriticalSection(&m_lock);

			m_list.Clear();

			::LeaveCriticalSection(&m_lock);
		}

	private:
		CRITICAL_SECTION    m_lock;
		PtrList<T>     m_list;
	};
}
