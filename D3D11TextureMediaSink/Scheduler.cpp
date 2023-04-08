#include "stdafx.h"

namespace D3D11TextureMediaSink
{
	Scheduler::Scheduler(CriticalSection* critSec)
	{
		this->_presentationSampleQueue = new ThreadSafeComPtrQueue<IMFSample>();
		this->_ScheduleEventQueue = new ThreadSafePtrQueue<ScheduleEvent>();
		this->_csStreamSinkAndScheduler = critSec;
		this->_frameInterval = 0;
		this->_quarterFrameInterval = 0;

		this->_MsgEvent = ::CreateEvent(NULL, FALSE, FALSE, NULL);
		this->_FlushCompleteEvent = ::CreateEvent(NULL, FALSE, FALSE, NULL);
	}
	Scheduler::~Scheduler()
	{
		this->Stop();
	}

	HRESULT Scheduler::SetFrameRate(const MFRatio& fps)
	{
		HRESULT hr;

		// Convert the input MFRatio to MFTIME and save it to the member variable.
		UINT64 avgTimePerFrame = 0;
		if (FAILED(hr = ::MFFrameRateToAverageTimePerFrame(fps.Numerator, fps.Denominator, &avgTimePerFrame)))
			return hr;

		this->_frameInterval = (MFTIME)avgTimePerFrame;
		this->_quarterFrameInterval = this->_frameInterval / 4;   // Pre-calculate 1/4 of the calculated MFTIME as it is commonly used.

		return S_OK;
	}
	HRESULT Scheduler::SetClockRate(float playbackRate)
	{
		this->_playbackRate = playbackRate;

		return S_OK;
	}

	HRESULT Scheduler::Start(IMFClock* pClock)
	{
		HRESULT hr = S_OK;

		// Update the presentation clock.
		SafeRelease(this->_PresentationClock);
		this->_PresentationClock = pClock;
		if (NULL != this->_PresentationClock)
			this->_PresentationClock->AddRef();

		// Start the scheduler thread.
		this->_threadStartNotification = ::CreateEvent(NULL, FALSE, FALSE, NULL);
		this->_threadHandle = ::CreateThreadpoolWork(&Scheduler::SchedulerThreadProcProxy, this, NULL);    // Allocate a worker thread
		::SubmitThreadpoolWork(this->_threadHandle);    // Start one worker thread
		DWORD r = ::WaitForSingleObject(this->_threadStartNotification, SCHEDULER_TIMEOUT);    // Wait for startup to complete
		this->_threadIsRunning = true;
		::CloseHandle(this->_threadStartNotification);

		return S_OK;
	}
	HRESULT Scheduler::Stop()
	{
		if (this->_threadIsRunning == FALSE)
			return S_FALSE; // not running

		// Set the termination event and notify the thread.
		this->_ScheduleEventQueue->Queue(new ScheduleEvent(eTerminate));
		::SetEvent(this->_MsgEvent);
		::WaitForThreadpoolWorkCallbacks(this->_threadHandle, FALSE);    // Wait for the worker thread to end
		this->_threadIsRunning = FALSE;

		// Destroy all samples in the queue.
		this->_presentationSampleQueue->Clear();

		// Release the presentation clock.
		SafeRelease(this->_PresentationClock);

		return S_OK;
	}
	HRESULT Scheduler::Flush()
	{
		AutoLock lock(this->_csStreamSinkAndScheduler);

		if (this->_threadIsRunning)
		{
			// Set a flush event and notify the thread.
			this->_ScheduleEventQueue->Queue(new ScheduleEvent(eFlush));
			::SetEvent(this->_MsgEvent);

			// Wait for the flush to complete.
			DWORD r = ::WaitForSingleObject(this->_FlushCompleteEvent, SCHEDULER_TIMEOUT);
			DWORD s = r;
		}

		return S_OK;
	}

	HRESULT Scheduler::ScheduleSample(IMFSample* pSample, BOOL bPresentNow)
	{
		if (NULL == this->_PresentCallback)
			return MF_E_NOT_INITIALIZED;

		HRESULT hr = S_OK;

		if (bPresentNow || (NULL == this->_PresentationClock))
		{
			// (A) Present the sample immediately.

			if (FAILED(hr = this->PresentSample(pSample)))
				return hr;
		}
		else
		{
			// (B) Store the sample in the presentation queue and notify the scheduler thread.

			if (FAILED(hr = pSample->SetUINT32(SAMPLE_STATE, SAMPLE_STATE_SCHEDULED)))
				return hr;

			this->_presentationSampleQueue->Queue(pSample);

			//TCHAR buf[1024];
			//wsprintf(buf, L"Scheduler::ScheduleSample - queued.(%X)\n", pSample);
			//OutputDebugString(buf);

			this->_ScheduleEventQueue->Queue(new ScheduleEvent(eSchedule));
			::SetEvent(this->_MsgEvent);
		}

		return S_OK;
	}

	void CALLBACK Scheduler::SchedulerThreadProcProxy(PTP_CALLBACK_INSTANCE pInstance, LPVOID arg, PTP_WORK pWork)
	{
		// Calls the private SchedulerThreadProcPrivate function.
		reinterpret_cast<Scheduler*>(arg)->SchedulerThreadProcPrivate();
	}
	UINT Scheduler::SchedulerThreadProcPrivate()
	{
		DWORD taskIndex = 0;
		::AvSetMmThreadCharacteristics(TEXT("Playback"), &taskIndex);

		// Signals the waiting thread.
		::SetEvent(this->_threadStartNotification);

		//OutputDebugString(L"Scheduler thread started.\n");

		DWORD lWait = INFINITE;
		BOOL bExitThread = FALSE;

		while (bExitThread == FALSE)
		{
			// Wait until (A)waitTime times out or (B)an event is caught.

			//_OutputDebugString(L"Scheduler::Thread, waiting with lWait = %d ms.\n", lWait);

			::WaitForSingleObject(this->_MsgEvent, lWait);


			if (0 == this->_ScheduleEventQueue->GetCount() && lWait != INFINITE)
			{
				//OutputDebugString(L"Scheduler::Thread, woken up by timeout.\n");

				// (A) When timed out, process samples at the scheduled time
				if (FAILED(this->ProcessSamplesInQueue(&lWait)))    // Get next wait time
				{
					bExitThread = TRUE; // Error occurred
					break;
				}
			}
			else
			{
				// (B) Process pending events one by one until the queue is empty

				BOOL bProcessSamples = TRUE;
				while (0 < this->_ScheduleEventQueue->GetCount())
				{
					ScheduleEvent* pEvent;
					if (FAILED(this->_ScheduleEventQueue->Dequeue(&pEvent)))
						break;	// Just in case

					switch (pEvent->Type)
					{
					case eTerminate:
						//OutputDebugString(L"Scheduler::Thread, processing eTerminate event.\n");
						bExitThread = TRUE;
						break;

					case eFlush:
						//OutputDebugString(L"Scheduler::Thread, processing eFlush event.\n");
						this->_presentationSampleQueue->Clear();	// Clear the queue
						lWait = INFINITE;
						::SetEvent(this->_FlushCompleteEvent); // Notify that flushing is complete
						break;

					case eSchedule:
						//OutputDebugString(L"Scheduler::Thread, processing eSchedule event.\n");
						if (bProcessSamples)
						{
							if (FAILED(this->ProcessSamplesInQueue(&lWait)))    // Process samples and get next wait time
							{
								bExitThread = TRUE;
								break;
							}
							bProcessSamples = (lWait != INFINITE);
						}
						break;
					}
				}
			}
		}

		//OutputDebugString(L"Thread ended.\n");
		return 0;
	}
	HRESULT Scheduler::ProcessSamplesInQueue(DWORD* plNextSleep)
	{
		HRESULT hr = S_OK;

		if (NULL == plNextSleep)
			return E_POINTER;

		*plNextSleep = 0;

		while (0 < this->_presentationSampleQueue->GetCount())
		{
			// Get a sample from the queue.
			IMFSample* pSample;
			if (FAILED(hr = this->_presentationSampleQueue->Dequeue(&pSample)))
				return hr;

			// Determine whether to display the sample.
			if (FAILED(hr = this->ProcessSample(pSample, plNextSleep)))
				return hr;

			if (0 < *plNextSleep)   // If it is not yet time to display,
			{
				this->_presentationSampleQueue->PutBack(pSample);  // put the sample back in the queue
				break;      // and exit the loop.
			}
		}

		if (0 == *plNextSleep)
			*plNextSleep = INFINITE;

		return hr;
	}
	HRESULT Scheduler::ProcessSample(IMFSample* pSample, DWORD* plNextSleep)
	{
		HRESULT hr = S_OK;
		*plNextSleep = 0;
		bool bPresentNow = true;

		if (NULL != this->_PresentationClock)   // Check if there is a clock
		{
			// Get the presentation time of the sample.
			LONGLONG hnsPresentationTime;
			if (FAILED(hr = pSample->GetSampleTime(&hnsPresentationTime)))
				hnsPresentationTime = 0;    // It is normal to have no timestamp.

			// Get the current time from the clock.
			LONGLONG hnsTimeNow;
			MFTIME hnsSystemTime;
			if (FAILED(hr = this->_PresentationClock->GetCorrelatedTime(0, &hnsTimeNow, &hnsSystemTime)))
				return hr;

			// Calculate the time until the presentation of the sample. A negative value means that the sample is delayed.
			LONGLONG hnsDelta = hnsPresentationTime - hnsTimeNow;
			if (0 > this->_playbackRate)
			{
				// In reverse playback, the clock also runs in reverse. Therefore, the time is reversed.
				hnsDelta = -hnsDelta;
			}

			// Check if the sample should be presented and calculate plNextSleep.
			if (hnsDelta < -this->_quarterFrameInterval)
			{
				// The sample is delayed, so it should be presented immediately.
				bPresentNow = true;
			}
			else if ((3 * this->_quarterFrameInterval) < hnsDelta)
			{
				// ‚±‚ÌƒTƒ“ƒvƒ‹‚Ì•\Ž¦‚Í‚Ü‚¾‘‚¢B
				*plNextSleep = this->MFTimeToMsec(hnsDelta - (3 * this->_quarterFrameInterval));
				*plNextSleep = ::abs((int)(*plNextSleep / this->_playbackRate));   // Reflect the playback rate.

				// Do not present yet.
				bPresentNow = false;

				if (*plNextSleep == 0)	// To avoid being treated as INFINITE when it is exactly 0.
					*plNextSleep = 1;

				//TCHAR buf[1024];
				//wsprintf(buf, L"Scheduler::ProcessSamples: This sample will not be presented yet. (%x)(lWait=%d)\n", pSample, *plNextSleep);
				//OutputDebugString(buf);
			}
		}

		// Present the sample.
		if (bPresentNow)
		{
			this->PresentSample(pSample);

			//TCHAR buf[1024];
			//wsprintf(buf, L"Scheduler::ProcessSamples: The sample has been presented. (%x)\n", pSample);
			//OutputDebugString(buf);
		}

		return S_OK;
	}
	int Scheduler::MFTimeToMsec(LONGLONG time)
	{
		const LONGLONG ONE_SECOND = 10000000; // One second in hns
		const int ONE_MSEC = 1000;       // One msec in hns

		return (int)(time / (ONE_SECOND / ONE_MSEC));
	}
	HRESULT Scheduler::PresentSample(IMFSample* pSample)
	{
		HRESULT hr = S_OK;

		// Callback invocation.
		this->_PresentCallback->PresentFrame(pSample);

		return S_OK;
	}
}
