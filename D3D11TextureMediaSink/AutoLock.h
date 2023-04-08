#pragma once

#include "CriticalSection.h"

namespace D3D11TextureMediaSink
{
	// Automatic scope of CriticalSection.
	// This instance locks the critical section in the constructor and releases it in the destructor.
	class AutoLock
	{
	public:
		_Acquires_lock_(this->m_pLock->m_cs)
		AutoLock(CriticalSection* pLock)
		{
			this->m_pLock = pLock;
			this->m_pLock->Lock();
		}

		_Releases_lock_(this->m_pLock->m_cs)
		~AutoLock()
		{
			this->m_pLock->Unlock();
		}

	private:
		CriticalSection* m_pLock;
	};
}
