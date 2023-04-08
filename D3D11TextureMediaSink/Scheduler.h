#pragma once

namespace D3D11TextureMediaSink
{
	struct SchedulerCallback
	{
		virtual HRESULT PresentFrame(IMFSample* pSample) = 0;
	};

	class Scheduler
	{
	public:
		Scheduler(CriticalSection* critSec);
		~Scheduler();

		void SetCallback(SchedulerCallback* pCB)
		{
			this->_PresentCallback = pCB;
		}
		HRESULT SetFrameRate(const MFRatio& fps);
		HRESULT SetClockRate(float playbackRate);

		HRESULT Start(IMFClock* pClock);
		HRESULT Stop();
		HRESULT Flush();

		HRESULT ScheduleSample(IMFSample* pSample, BOOL bPresentNow);

	private:
		const int SCHEDULER_TIMEOUT = 5000; // 5 seconds
		const int _INFINITE = -1;

		CriticalSection* _csStreamSinkAndScheduler;
		SchedulerCallback*  _PresentCallback = NULL;
		MFTIME _frameInterval;
		LONGLONG _quarterFrameInterval;		// Precomputed for frequent use
		float _playbackRate = 1.0f;
		IMFClock* _PresentationClock = NULL;	// Can be set to NULL

		enum ScheduleEventType
		{
			eTerminate,
			eSchedule,
			eFlush,
		};
		class ScheduleEvent
		{
		public:
			ScheduleEventType Type;
			ScheduleEvent(ScheduleEventType type)
			{
				this->Type = type;
			}
		};
		ThreadSafeComPtrQueue<IMFSample>* _presentationSampleQueue;
		BOOL _threadIsRunning = FALSE;
		PTP_WORK _threadHandle = NULL;
		HANDLE _threadStartNotification;
		ThreadSafePtrQueue<ScheduleEvent>* _ScheduleEventQueue;
		HANDLE _MsgEvent = NULL;
		HANDLE _FlushCompleteEvent = NULL;

		static void CALLBACK SchedulerThreadProcProxy(PTP_CALLBACK_INSTANCE pInstance, void* arg, PTP_WORK pWork);
		UINT SchedulerThreadProcPrivate();
		HRESULT ProcessSamplesInQueue(DWORD* plNextSleep);
		HRESULT ProcessSample(IMFSample* pSample, DWORD* plNextSleep);
		int MFTimeToMsec(LONGLONG time);
		HRESULT PresentSample(IMFSample* pSample);
	};
}
