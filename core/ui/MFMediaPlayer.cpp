//////////////////////////////////////////////////////////////////////////
//
// MFMediaPlayer.cpp : Playback helper class.
//
// THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
// PARTICULAR PURPOSE.
//
// Copyright (c) Microsoft Corporation. All rights reserved.
//
//////////////////////////////////////////////////////////////////////////

#include "MFMediaPlayer.h"
#include <Shlwapi.h>

// #pragma comment(lib, "evr") // MR_VIDEO_RENDER_SERVICE required
#pragma comment(lib, "mf")
#pragma comment(lib, "mfplat")
#pragma comment(lib, "mfplay")
#pragma comment(lib, "mfuuid")
#pragma comment(lib, "shlwapi")

// IF_FAILED_GOTO macro.
// Jumps to 'label' on failure.
#ifndef IF_FAILED_GOTO
#    define IF_FAILED_GOTO(hr, label) \
        if (FAILED(hr))               \
        {                             \
            goto label;               \
        }
#endif

#define CHECK_HR(hr) IF_FAILED_GOTO(hr, done)

#define TRACE(...)

// Forward declarations

HRESULT CreateSourceStreamNode(IMFMediaSource* pSource,
                               IMFPresentationDescriptor* pSourcePD,
                               IMFStreamDescriptor* pSourceSD,
                               IMFTopologyNode** ppNode);

HRESULT CreateOutputNode(IMFStreamDescriptor* pSourceSD, HWND hwndVideo, IMFTopologyNode** ppNode);

///////////////////////////////////////////////////////////////////////
//  Name: CreateInstance
//  Description:  Static class method to create the MFMediaPlayer object.
//
//  hVideo:   Handle to the video window.
//  hEvent:   Handle to the window to receive notifications.
//  ppPlayer: Receives an AddRef's pointer to the MFMediaPlayer object.
//            The caller must release the pointer.
/////////////////////////////////////////////////////////////////////////

HRESULT MFMediaPlayer::CreateInstance(HWND hEvent, MFMediaPlayer** ppPlayer)
{
    TRACE((L"MFMediaPlayer::Create\n"));

    assert(hEvent != NULL);

    if (ppPlayer == NULL)
    {
        return E_POINTER;
    }

    HRESULT hr = S_OK;

    auto pPlayer = new MFMediaPlayer(hEvent);

    if (pPlayer == NULL)
    {
        return E_OUTOFMEMORY;
    }

    hr = pPlayer->Initialize();

    if (SUCCEEDED(hr))
        *ppPlayer = pPlayer;
    else
        pPlayer->Release();

    return hr;
}

///////////////////////////////////////////////////////////////////////
//  MFMediaPlayer constructor
/////////////////////////////////////////////////////////////////////////

MFMediaPlayer::MFMediaPlayer(HWND hEvent)
    : m_pSession(NULL), m_pSource(NULL), m_hwndEvent(hEvent), m_state(Ready), m_hCloseEvent(NULL), m_nRefCount(1)
{}

///////////////////////////////////////////////////////////////////////
//  MFMediaPlayer destructor
/////////////////////////////////////////////////////////////////////////

MFMediaPlayer::~MFMediaPlayer()
{
    assert(m_pSession == NULL);  // If FALSE, the app did not call Shutdown().

    // Note: The application must call Shutdown() because the media
    // session holds a reference count on the MFMediaPlayer object. (This happens
    // when MFMediaPlayer calls IMediaEventGenerator::BeginGetEvent on the
    // media session.) As a result, there is a circular reference count
    // between the MFMediaPlayer object and the media session. Calling Shutdown()
    // breaks the circular reference count.

    // Note: If CreateInstance failed, the application will not call
    // Shutdown(). To handle that case, we must call Shutdown() in the
    // destructor. The circular ref-count problem does not occcur if
    // CreateInstance has failed. Also, calling Shutdown() twice is
    // harmless.

    Shutdown();
}

//////////////////////////////////////////////////////////////////////
//  Name: Initialize
//  Initializes the MFMediaPlayer object. This method is called by the
//  CreateInstance method.
/////////////////////////////////////////////////////////////////////////

HRESULT MFMediaPlayer::Initialize()
{
    HRESULT hr = S_OK;

    if (m_hCloseEvent)
    {
        return MF_E_ALREADY_INITIALIZED;
    }

    // Start up Media Foundation platform.
    CHECK_HR(hr = MFStartup(MF_VERSION));

    m_hCloseEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (m_hCloseEvent == NULL)
    {
        CHECK_HR(hr = HRESULT_FROM_WIN32(GetLastError()));
    }

done:
    return hr;
}

///////////////////////////////////////////////////////////////////////
//  AddRef
/////////////////////////////////////////////////////////////////////////

ULONG MFMediaPlayer::AddRef()
{
    return InterlockedIncrement(&m_nRefCount);
}

///////////////////////////////////////////////////////////////////////
//  Release
/////////////////////////////////////////////////////////////////////////

ULONG MFMediaPlayer::Release()
{
    ULONG uCount = InterlockedDecrement(&m_nRefCount);
    if (uCount == 0)
    {
        delete this;
    }
    // For thread safety, return a temporary variable.
    return uCount;
}

///////////////////////////////////////////////////////////////////////
//  QueryInterface
/////////////////////////////////////////////////////////////////////////

HRESULT MFMediaPlayer::QueryInterface(REFIID iid, void** ppv)
{
    if (!ppv)
    {
        return E_POINTER;
    }
    if (iid == IID_IUnknown)
    {
        *ppv = static_cast<IUnknown*>(this);
    }
    else if (iid == IID_IMFAsyncCallback)
    {
        *ppv = static_cast<IMFAsyncCallback*>(this);
    }
    else
    {
        *ppv = NULL;
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

///////////////////////////////////////////////////////////////////////
//  Name: OpenURL
//  Description:  Opens a URL for playback.
/////////////////////////////////////////////////////////////////////////

HRESULT MFMediaPlayer::OpenURL(const WCHAR* sURL)
{
    TRACE((L"MFMediaPlayer::OpenURL\n"));
    TRACE((L"URL = %s\n", sURL));

    // 1. Create a new media session.
    // 2. Create the media source.
    // 3. Create the topology.
    // 4. Queue the topology [asynchronous]
    // 5. Start playback [asynchronous - does not happen in this method.]

    HRESULT hr = S_OK;
    TComPtr<IMFTopology> pTopology;
    TComPtr<IMFClock> pClock;

    // Create the media session.
    CHECK_HR(hr = CreateSession());

    // Create the media source.
    CHECK_HR(hr = CreateMediaSource(sURL));

    // Create a partial topology.
    CHECK_HR(hr = CreateTopologyFromSource(&pTopology));

    // Set the topology on the media session.
    CHECK_HR(hr = m_pSession->SetTopology(0, pTopology));

    // If SetTopology succeeded, the media session will queue an
    // MESessionTopologySet event.

    // ======> Read media properties
    // Get the session capabilities.
    CHECK_HR(hr = m_pSession->GetSessionCapabilities(&m_caps));

    // Get the duration from the presentation descriptor (optional)
    (void)m_PresentDescriptor->GetUINT64(MF_PD_DURATION, (UINT64*)&m_hnsDuration);

    // Get the presentation clock (optional)
    hr = m_pSession->GetClock(&pClock);
    if (SUCCEEDED(hr))
        CHECK_HR(hr = pClock->QueryInterface(IID_PPV_ARGS(&m_pClock)));

    // Get the rate control interface (optional)
    CHECK_HR(hr = MFGetService(m_pSession, MF_RATE_CONTROL_SERVICE, IID_PPV_ARGS(&m_RateControl)));

    CHECK_HR(hr = MFGetService(m_pSession, MF_RATE_CONTROL_SERVICE, IID_PPV_ARGS(&m_RateSupport)));

    // Check if rate 0 (scrubbing) is supported.
    auto hrTmp = m_RateSupport->IsRateSupported(TRUE, 0, NULL);
    if (SUCCEEDED(hrTmp))
        m_bCanScrub = TRUE;

    // if m_pRate is NULL, m_bCanScrub must be FALSE.
    assert(m_RateControl || !m_bCanScrub);

    // Set our state to "open pending"
    m_state = OpenPending;

done:
    if (FAILED(hr))
    {
        m_state = Closed;
    }

    // SAFE_RELEASE(pTopology);

    return hr;
}

///////////////////////////////////////////////////////////////////////
//  Name: Invoke
//  Description:  Callback for asynchronous BeginGetEvent method.
//
//  pAsyncResult: Pointer to the result.
/////////////////////////////////////////////////////////////////////////

HRESULT MFMediaPlayer::Invoke(IMFAsyncResult* pResult)
{
    HRESULT hr            = S_OK;
    MediaEventType meType = MEUnknown;  // Event type

    TComPtr<IMFMediaEvent> pEvent;

    // Get the event from the event queue.
    CHECK_HR(hr = m_pSession->EndGetEvent(pResult, &pEvent));

    // Get the event type.
    CHECK_HR(hr = pEvent->GetType(&meType));

    // If the session is closed, the application is waiting on the
    // m_hCloseEvent event handle. Also, do not get any more
    // events from the session.

    if (meType == MESessionClosed)
    {
        SetEvent(m_hCloseEvent);
    }
    else
    {
        // For all other events, ask the media session for the
        // next event in the queue.
        CHECK_HR(hr = m_pSession->BeginGetEvent(this, NULL));
    }

    // For most events, we post the event as a private window message to the
    // application. This lets the application process the event on it's
    // main thread.

    // However, if call to IMFMediaSession::Close is pending, it means the
    // application is waiting on the m_hCloseEvent event handle. (Blocking
    // call.) In that case, we simply discard the event.

    // NOTE: When IMFMediaSession::Close is called, MESessionClosed is NOT
    // necessarily the next event that we will receive. We may receive
    // any number of other events before receiving MESessionClosed.
    if (m_state != Closing)
    {
        // Leave a reference count on the event.
        pEvent->AddRef();
        // PostMessage(m_hwndEvent, WM_APP_PLAYER_EVENT, (WPARAM)pEvent.Get(), (LPARAM)0);
        // OnPlayerEvent(hwnd, wParam);
        HRESULT hr = HandleEvent((WPARAM)pEvent.Get());
    }

done:
    // SAFE_RELEASE(pEvent);
    return S_OK;
}

//-------------------------------------------------------------------
//  HandleEvent
//
//  Called by the application when it receives a WM_APP_PLAYER_EVENT
//  message.
//
//  This method is used to process media session events on the
//  application's main thread.
//
//  pUnkPtr: Pointer to the IUnknown interface of a media session
//  event (IMFMediaEvent).
//-------------------------------------------------------------------

HRESULT MFMediaPlayer::HandleEvent(UINT_PTR pUnkPtr)
{
    HRESULT hr               = S_OK;
    HRESULT hrStatus         = S_OK;                   // Event status
    MediaEventType meType    = MEUnknown;              // Event type
    MF_TOPOSTATUS TopoStatus = MF_TOPOSTATUS_INVALID;  // Used with MESessionTopologyStatus event.

    TComPtr<IUnknown> pUnk{(IUnknown*)pUnkPtr};
    TComPtr<IMFMediaEvent> pEvent;

    PROPVARIANT var;

    // pUnkPtr is really an IUnknown pointer.
    // pUnk = (IUnknown*)pUnkPtr;

    if (!pUnk)
    {
        return E_POINTER;
    }

    CHECK_HR(hr = pUnk->QueryInterface(__uuidof(IMFMediaEvent), (void**)&pEvent));

    // Get the event type.
    CHECK_HR(hr = pEvent->GetType(&meType));

    // Get the event status. If the operation that triggered the event did
    // not succeed, the status is a failure code.
    CHECK_HR(hr = pEvent->GetStatus(&hrStatus));

    TRACE((L"Media event: %s\n", EventName(meType)));

    // Check if the async operation succeeded.
    if (SUCCEEDED(hrStatus))
    {
        // Switch on the event type. Update the internal state of the MFMediaPlayer as needed.
        switch (meType)
        {
        case MESessionTopologyStatus:
            // Get the status code.
            CHECK_HR(hr = pEvent->GetUINT32(MF_EVENT_TOPOLOGY_STATUS, (UINT32*)&TopoStatus));
            switch (TopoStatus)
            {
            case MF_TOPOSTATUS_READY:
                hr = OnTopologyReady(pEvent);
                break;
            default:
                // Nothing to do.
                break;
            }
            break;
        case MEEndOfPresentation:
            CHECK_HR(hr = OnPlayEnded(pEvent));
            break;
        case MESessionStarted:
            OnSessionStart(hrStatus);
            break;

        case MESessionStopped:
            OnSessionStop(hrStatus);
            break;

        case MESessionPaused:
            OnSessionPause(hrStatus);
            break;

        case MESessionRateChanged:
            // If the rate change succeeded, we've already got the rate
            // cached. If it failed, try to get the actual rate.
            if (FAILED(hrStatus))
            {
                PropVariantInit(&var);

                hr = pEvent->GetValue(&var);

                if (SUCCEEDED(hr) && (var.vt == VT_R4))
                {
                    m_nominal.fRate = var.fltVal;
                }
            }
            break;

        case MESessionEnded:
            OnSessionEnded(hrStatus);
            break;

        case MESessionCapabilitiesChanged:
            // The session capabilities changed. Get the updated capabilities.
            m_caps = MFGetAttributeUINT32(pEvent, MF_EVENT_SESSIONCAPS, m_caps);
            break;
        }
    }
    else
    {
        hr = hrStatus;
    }

done:
    // SAFE_RELEASE(pUnk);
    // SAFE_RELEASE(pEvent);
    return hr;
}

///////////////////////////////////////////////////////////////////////
//  Name: ShutDown
//  Description:  Releases all resources held by this object.
/////////////////////////////////////////////////////////////////////////

HRESULT MFMediaPlayer::Shutdown()
{
    TRACE((L"MFMediaPlayer::ShutDown\n"));

    HRESULT hr = S_OK;

    // Close the session
    hr = CloseSession();

    // Shutdown the Media Foundation platform
    MFShutdown();

    if (m_hCloseEvent)
    {
        CloseHandle(m_hCloseEvent);
        m_hCloseEvent = NULL;
    }

    return hr;
}

///////////////////////////////////////////////////////////////////////
//  Name: GetContentProtectionManager
//  Description:  Returns the content protection manager object.
//
//  This is a helper object for handling IMFContentEnabler operations.
/////////////////////////////////////////////////////////////////////////

// HRESULT  MFMediaPlayer::GetContentProtectionManager(ContentProtectionManager **ppManager)
//{
//    if (ppManager == NULL)
//    {
//        return E_INVALIDARG;
//    }
//
//    if (m_pContentProtectionManager == NULL)
//    {
//        return E_FAIL; // Session wasn't created yet. No helper object;
//    }
//
//    *ppManager = m_pContentProtectionManager;
//    (*ppManager)->AddRef();
//
//    return S_OK;
//
//}

///
///
/// Protected methods
///
/// All methods that follow are private to the MFMediaPlayer class.
///
///

///////////////////////////////////////////////////////////////////////
//  Name: OnTopologyReady
//  Description:  Handler for MESessionTopologyReady event.
//
//  Note:
//  - The MESessionTopologySet event means the session queued the
//    topology, but the topology is not ready yet. Generally, the
//    applicationno need to respond to this event unless there is an
//    error.
//  - The MESessionTopologyReady event means the new topology is
//    ready for playback. After this event is received, any calls to
//    IMFGetService will get service interfaces from the new topology.
/////////////////////////////////////////////////////////////////////////

HRESULT MFMediaPlayer::OnTopologyReady(IMFMediaEvent* pEvent)
{
    TRACE((L"MFMediaPlayer::OnTopologyReady\n"));

    if (m_bPlayOnOpen)
        StartPlayback(nullptr);

    return S_OK;
}

// Starts playback.

HRESULT MFMediaPlayer::Play()
{
    HRESULT hr = S_OK;

    TRACE((L"MFMediaPlayer::Play\n"));

    if (m_state != Paused && m_state != Stopped)
    {
        return MF_E_INVALIDREQUEST;
    }
    if (m_pSession == NULL || m_pSource == NULL)
    {
        return E_UNEXPECTED;
    }

    AutoLock lock(m_critsec);

    // If another operation is pending, cache the request.
    // Otherwise, start the media session.
    if (m_bPending)
    {
        m_request.command = CmdStart;
    }
    else
    {
        hr = StartPlayback(nullptr);

        m_nominal.command = CmdStart;
        m_bPending        = CMD_PENDING;
    }

    return hr;
}

// Pauses playback.

HRESULT MFMediaPlayer::Pause()
{
    HRESULT hr = S_OK;

    AutoLock lock(m_critsec);

    // If another operation is pending, cache the request.
    // Otherwise, pause the media session.
    if (m_bPending)
    {
        m_request.command = CmdPause;
    }
    else
    {
        hr = m_pSession->Pause();

        m_nominal.command = CmdPause;
        m_bPending        = CMD_PENDING;
    }

    if (SUCCEEDED(hr))
        m_state = Paused;

    return hr;
}

// Stops playback.

HRESULT MFMediaPlayer::Stop()
{
    HRESULT hr = S_OK;

    AutoLock lock(m_critsec);

    // If another operation is pending, cache the request.
    // Otherwise, stop the media session.
    if (m_bPending)
    {
        m_request.command = CmdStop;
    }
    else
    {
        hr = m_pSession->Stop();

        m_nominal.command = CmdStop;
        m_bPending        = CMD_PENDING;
    }
    return hr;
}

BOOL MFMediaPlayer::CanSeek() const
{
    bool bCanSeek = (((m_caps & MFSESSIONCAP_SEEK) == MFSESSIONCAP_SEEK) && (m_hnsDuration > 0) && (m_pClock != NULL));
    return bCanSeek;
}

MFTIME MFMediaPlayer::GetDuration() const
{
    return m_hnsDuration;
}

MFTIME MFMediaPlayer::GetCurrentPosition() const
{
    HRESULT hr = S_OK;

    // TODO:
    AutoLock lock(m_critsec);

    MFTIME hnsPosition = 0;

    if (m_pClock == NULL)
    {
        return MF_E_NO_CLOCK;
    }

    // Return, in order:
    // 1. Cached seek request (nominal position).
    // 2. Pending seek operation (nominal position).
    // 3. Presentation time (actual position).

    if (m_request.command == CmdSeek)
    {
        hnsPosition = m_request.hnsStart;
    }
    else if (m_bPending & CMD_PENDING_SEEK)
    {
        hnsPosition = m_nominal.hnsStart;
    }
    else
    {
        hr = m_pClock->GetTime(&hnsPosition);
    }

    return hnsPosition;
}

HRESULT MFMediaPlayer::SetPosition(MFTIME hnsPosition)
{
    AutoLock lock(m_critsec);

    HRESULT hr = S_OK;

    if (m_bPending)
    {
        // Currently seeking or changing rates, so cache this request.
        m_request.command  = CmdSeek;
        m_request.hnsStart = hnsPosition;
    }
    else
    {
        hr = SetPositionInternal(hnsPosition);
    }

    return hr;
}

BOOL MFMediaPlayer::CanScrub() const
{
    return m_bCanScrub;
}

HRESULT MFMediaPlayer::Scrub(BOOL bScrub)
{
    // Scrubbing is implemented as rate = 0.

    AutoLock lock(m_critsec);

    if (!m_RateControl)
    {
        return MF_E_INVALIDREQUEST;
    }
    if (!m_bCanScrub)
    {
        return MF_E_INVALIDREQUEST;
    }

    HRESULT hr = S_OK;

    if (bScrub)
    {
        // Enter scrubbing mode. Cache the current rate.

        if (GetNominalRate() != 0)
        {
            m_fPrevRate = m_nominal.fRate;
        }

        hr = SetRate(0.0f);
    }
    else
    {
        // Leaving scrubbing mode. Restore the old rate.

        if (GetNominalRate() == 0)
        {
            hr = SetRate(m_fPrevRate);
        }
    }

    return hr;
}

BOOL MFMediaPlayer::CanFastForward() const
{
    BOOL bCanFF = ((m_caps & MFSESSIONCAP_RATE_FORWARD) == MFSESSIONCAP_RATE_FORWARD);
    return bCanFF;
}

BOOL MFMediaPlayer::CanRewind() const
{
    BOOL bCanRewind = ((m_caps & MFSESSIONCAP_RATE_REVERSE) == MFSESSIONCAP_RATE_REVERSE);
    return bCanRewind;
}

HRESULT MFMediaPlayer::FastForward()
{
    if (!m_RateControl)
    {
        return MF_E_INVALIDREQUEST;
    }

    HRESULT hr    = S_OK;
    float fTarget = GetNominalRate() * 2;

    if (fTarget <= 0.0f)
    {
        fTarget = 1.0f;
    }

    hr = SetRate(fTarget);

    return hr;
}

HRESULT MFMediaPlayer::Rewind()
{
    if (!m_RateControl)
    {
        return MF_E_INVALIDREQUEST;
    }

    HRESULT hr    = S_OK;
    float fTarget = GetNominalRate() * 2;

    if (fTarget >= 0.0f)
    {
        fTarget = -1.0f;
    }

    hr = SetRate(fTarget);

    return hr;
}

HRESULT MFMediaPlayer::SetRate(float fRate)
{
    HRESULT hr = S_OK;
    BOOL bThin = FALSE;

    AutoLock lock(m_critsec);

    if (fRate == GetNominalRate())
    {
        return S_OK;  // no-op
    }

    if (m_RateSupport == NULL)
    {
        return MF_E_INVALIDREQUEST;
    }

    // Check if this rate is supported. Try non-thinned playback first,
    // then fall back to thinned playback.

    hr = m_RateSupport->IsRateSupported(FALSE, fRate, NULL);
    if (FAILED(hr) && hr != MF_E_INVALIDREQUEST)
    {
        bThin = TRUE;
        hr    = m_RateSupport->IsRateSupported(TRUE, fRate, NULL);
    }

    if (FAILED(hr) && hr != MF_E_INVALIDREQUEST)
    {
        // Unsupported rate.
        return hr;
    }

    // If there is an operation pending, cache the request.
    if (m_bPending || hr == MF_E_INVALIDREQUEST)
    {
        m_bPending = CMD_PENDING_RATE;

        m_request.fRate = fRate;
        m_request.bThin = bThin;

        // Remember the current transport state (play, paused, etc), so that we
        // can restore it after the rate change, if necessary. However, if
        // anothercommand is already pending, that one takes precedent.

        if (m_request.command == CmdNone)
        {
            m_request.command = m_nominal.command;
        }
    }
    else
    {
        // No pending operation. Commit the new rate.
        hr = CommitRateChange(fRate, bThin);
    }

    return hr;
}

HRESULT MFMediaPlayer::SetPositionInternal(const MFTIME& hnsPosition)
{
    assert(!m_bPending);

    if (m_pSession == NULL)
    {
        return MF_E_INVALIDREQUEST;
    }

    HRESULT hr = StartPlayback(&hnsPosition);
    if (SUCCEEDED(hr))
    {
        // Store the pending state.
        m_nominal.command  = CmdStart;
        m_nominal.hnsStart = hnsPosition;
        m_bPending         = CMD_PENDING_SEEK;

        m_state = Started;
    }
    return hr;
}

HRESULT MFMediaPlayer::StartPlayback(const MFTIME* hnsPosition)
{
    HRESULT hr = S_OK;

    PROPVARIANT varStart;
    if (!hnsPosition)
        varStart.vt = VT_EMPTY;
    else
    {
        varStart.vt            = VT_I8;
        varStart.hVal.QuadPart = *hnsPosition;
    }

    // Try apply pending rate before start playback
    if (m_bPending & CMD_PENDING_RATE && m_request.fRate != m_nominal.fRate)
    {
        if (SUCCEEDED(m_RateControl->SetRate(m_request.bThin, m_request.fRate)))
        {
            m_bPending        = FALSE;
            m_request.command = CmdNone;
            m_nominal.fRate   = m_request.fRate;
        }
    }

    hr = m_pSession->Start(NULL, &varStart);

    // Note: Start is an asynchronous operation. However, we
    // can treat our state as being already started. If Start
    // fails later, we'll get an MESessionStarted event with
    // an error code, and we will update our state then.
    if (SUCCEEDED(hr))
        m_state = Started;
    return hr;
}

HRESULT MFMediaPlayer::CommitRateChange(float fRate, BOOL bThin)
{
    assert(!m_bPending);

    // Caller holds the lock.

    HRESULT hr           = S_OK;
    MFTIME hnsSystemTime = 0;
    MFTIME hnsClockTime  = 0;

    Command cmdNow = m_nominal.command;

    TComPtr<IMFClock> pClock;

    // Allowed rate transitions:

    // Positive <-> negative:   Stopped
    // Negative <-> zero:       Stopped
    // Postive <-> zero:        Paused or stopped

    if ((fRate > 0 && m_nominal.fRate <= 0) || (fRate < 0 && m_nominal.fRate >= 0))
    {
        // Transition to stopped.
        if (cmdNow == CmdStart)
        {
            // Get the current clock position. This will be the restart time.
            hr = m_pSession->GetClock(&pClock);
            if (FAILED(hr))
            {
                goto done;
            }

            (void)pClock->GetCorrelatedTime(0, &hnsClockTime, &hnsSystemTime);

            assert(hnsSystemTime != 0);

            // Stop and set the rate
            hr = Stop();
            if (FAILED(hr))
            {
                goto done;
            }

            // Cache Request: Restart from stop.
            m_request.command  = CmdSeek;
            m_request.hnsStart = hnsClockTime;
        }
        else if (cmdNow == CmdPause)
        {
            // The current state is paused.

            // For this rate change, the session must be stopped. However, the
            // session cannot transition back from stopped to paused.
            // Therefore, this rate transition is not supported while paused.

            hr = MF_E_UNSUPPORTED_STATE_TRANSITION;
            goto done;
        }
    }
    else if (fRate == 0 && m_nominal.fRate != 0)
    {
        if (cmdNow != CmdPause)
        {
            // Transition to paused.

            // This transisition requires the paused state.

            // Pause and set the rate.
            hr = Pause();
            if (FAILED(hr))
            {
                goto done;
            }

            // Request: Switch back to current state.
            m_request.command = cmdNow;
        }
    }

    // Set the rate.
    hr = m_RateControl->SetRate(bThin, fRate);
    if (FAILED(hr))
    {
        goto done;
    }

    // Adjust our current rate and requested rate.
    m_request.fRate = m_nominal.fRate = fRate;

done:
    return hr;
}
float MFMediaPlayer::GetNominalRate() const
{
    return m_request.fRate;
}

HRESULT MFMediaPlayer::UpdatePendingCommands(Command cmd)
{
    HRESULT hr = S_OK;

    PROPVARIANT varStart;
    PropVariantInit(&varStart);

    AutoLock lock(m_critsec);

    if (m_bPending && m_nominal.command == cmd)
    {
        m_bPending = FALSE;

        // The current pending command has completed.

        // First look for rate changes.
        if (m_request.fRate != m_nominal.fRate)
        {
            hr = CommitRateChange(m_request.fRate, m_request.bThin);
            if (FAILED(hr))
            {
                goto done;
            }
        }

        // Now look for seek requests.
        if (!m_bPending)
        {
            switch (m_request.command)
            {
            case CmdNone:
                // Nothing to do.
                break;

            case CmdStart:
                Play();
                break;

            case CmdPause:
                Pause();
                break;

            case CmdStop:
                Stop();
                break;

            case CmdSeek:
                SetPositionInternal(m_request.hnsStart);
                break;
            }
            m_request.command = CmdNone;
        }
    }

done:
    return hr;
}

HRESULT MFMediaPlayer::OnPlayEnded(IMFMediaEvent* pEvent)
{
    TRACE((L"MFMediaPlayer::OnPlayEnded\n"));

    // The session puts itself into the stopped state autmoatically.

    m_state = Stopped;

    if (m_bLooping)
    {
        SetPosition(0);
    }

    return S_OK;
}

HRESULT MFMediaPlayer::OnSessionStart(HRESULT hrStatus)
{
    HRESULT hr = S_OK;

    if (FAILED(hrStatus))
    {
        return hrStatus;
    }

    // The Media Session completed a start/seek operation. Check if there
    // is another seek request pending.
    UpdatePendingCommands(CmdStart);

    return hr;
}

HRESULT MFMediaPlayer::OnSessionStop(HRESULT hrStatus)
{
    HRESULT hr = S_OK;

    if (FAILED(hrStatus))
    {
        return hrStatus;
    }

    // The Media Session completed a transition to stopped. This might occur
    // because we are changing playback direction (forward/rewind). Check if
    // there is a pending rate-change request.

    UpdatePendingCommands(CmdStop);

    return hr;
}

HRESULT MFMediaPlayer::OnSessionPause(HRESULT hrStatus)
{
    HRESULT hr = S_OK;

    if (FAILED(hrStatus))
    {
        return hrStatus;
    }

    hr = UpdatePendingCommands(CmdPause);

    return hr;
}

HRESULT MFMediaPlayer::OnSessionEnded(HRESULT hrStatus)
{  // After the session ends, playback starts from position zero. But if the
    // current playback rate is reversed, playback would end immediately
    // (reversing from position 0). Therefore, reset the rate to 1x.

    if (GetNominalRate() < 0.0f)
    {
        m_nominal.command = CmdStop;

        hrStatus = CommitRateChange(1.0f, FALSE);
    }

    return hrStatus;
}

///////////////////////////////////////////////////////////////////////
//  Name: CreateSession
//  Description:  Creates a new instance of the media session.
/////////////////////////////////////////////////////////////////////////

HRESULT MFMediaPlayer::CreateSession()
{
    TRACE((L"MFMediaPlayer::CreateSession\n"));

    HRESULT hr = S_OK;

    TComPtr<IMFAttributes> pAttributes;
    // IMFActivate   *pEnablerActivate = NULL;

    // Close the old session, if any.
    CHECK_HR(hr = CloseSession());

    assert(m_state == Closed);

    // Create a new attribute store.
    CHECK_HR(hr = MFCreateAttributes(&pAttributes, 2));

    pAttributes->SetUINT32(MF_LOW_LATENCY, TRUE);
#if 0
	// pAttributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);

	// Create the content protection manager.
	assert(m_pContentProtectionManager == NULL); // Was released in CloseSession

	CHECK_HR(hr = ContentProtectionManager::CreateInstance(
		m_hwndEvent,
		&m_pContentProtectionManager
	));

	// Set the MF_SESSION_CONTENT_PROTECTION_MANAGER attribute with a pointer
	// to the content protection manager.
	CHECK_HR(hr = pAttributes->SetUnknown(
		MF_SESSION_CONTENT_PROTECTION_MANAGER,
		(IMFContentProtectionManager*)m_pContentProtectionManager
	));


	// Create the PMP media session.
	CHECK_HR(hr = MFCreatePMPMediaSession(
		0, // Can use this flag: MFPMPSESSION_UNPROTECTED_PROCESS
		pAttributes,
		&m_pSession,
		&pEnablerActivate
	));
#endif
    CHECK_HR(hr = MFCreateMediaSession(pAttributes, &m_pSession));

    // TODO:

    // If MFCreatePMPMediaSession fails it might return an IMFActivate pointer.
    // This indicates that a trusted binary failed to load in the protected process.
    // An application can use the IMFActivate pointer to create an enabler object, which
    // provides revocation and renewal information for the component that failed to
    // load.

    // This sample does not demonstrate that feature. Instead, we simply treat this
    // case as a playback failure.

    // Start pulling events from the media session
    CHECK_HR(hr = m_pSession->BeginGetEvent((IMFAsyncCallback*)this, NULL));

done:
    // SAFE_RELEASE(pAttributes);
    // SAFE_RELEASE(pEnablerActivate);
    return hr;
}

///////////////////////////////////////////////////////////////////////
//  Name: CloseSession
//  Description:  Closes the media session.
//
//  Note: The IMFMediaSession::Close method is asynchronous, but the
//        MFMediaPlayer::CloseSession method waits on the MESessionClosed event.
//        The MESessionClosed event is guaranteed to be the last event
//        that the media session fires.
/////////////////////////////////////////////////////////////////////////

HRESULT MFMediaPlayer::CloseSession()
{
    HRESULT hr = S_OK;

    // SAFE_RELEASE(m_pVideoDisplay);

    if (m_pSession)
    {
        DWORD dwWaitResult = 0;

        m_state = Closing;

        CHECK_HR(hr = m_pSession->Close());

        // Wait for the close operation to complete

        dwWaitResult = WaitForSingleObject(m_hCloseEvent, 5000);

        if (dwWaitResult == WAIT_TIMEOUT)
        {
            TRACE((L"CloseSession timed out!\n"));
        }

        // Now there will be no more events from this session.
    }

    // Complete shutdown operations.

    // Shut down the media source. (Synchronous operation, no events.)
    if (m_pSource)
    {
        m_pSource->Shutdown();
    }

    // Shut down the media session. (Synchronous operation, no events.)
    if (m_pSession)
    {
        m_pSession->Shutdown();
    }

    m_PresentDescriptor.Reset();
    m_RateControl.Reset();
    m_RateSupport.Reset();
    m_pClock.Reset();

    m_pSource.Reset();
    m_pSession.Reset();

    m_state = Closed;

done:
    return hr;
}

///////////////////////////////////////////////////////////////////////
//  Name: CreateMediaSource
//  Description:  Create a media source from a URL.
//
//  sURL: The URL to open.
/////////////////////////////////////////////////////////////////////////

HRESULT MFMediaPlayer::CreateMediaSource(const WCHAR* sURL)
{
    TRACE((L"MFMediaPlayer::CreateMediaSource\n"));

    HRESULT hr                = S_OK;
    MF_OBJECT_TYPE ObjectType = MF_OBJECT_INVALID;

    TComPtr<IMFSourceResolver> pSourceResolver;
    TComPtr<IUnknown> pSource;

    m_pSource.Reset();  // SAFE_RELEASE(m_pSource);

    // Create the source resolver.
    CHECK_HR(hr = MFCreateSourceResolver(&pSourceResolver));

    // Use the source resolver to create the media source.

    // Note: For simplicity this sample uses the synchronous method on
    // IMFSourceResolver to create the media source. However, creating a
    // media source can take a noticeable amount of time, especially for
    // a network source. For a more responsive UI, use the asynchronous
    // BeginCreateObjectFromURL method.

    CHECK_HR(hr = pSourceResolver->CreateObjectFromURL(sURL,                       // URL of the source.
                                                       MF_RESOLUTION_MEDIASOURCE,  // Create a source object.
                                                       NULL,                       // Optional property store.
                                                       &ObjectType,                // Receives the created object type.
                                                       &pSource  // Receives a pointer to the media source.
                                                       ));

    // Get the IMFMediaSource interface from the media source.
    CHECK_HR(hr = pSource->QueryInterface(__uuidof(IMFMediaSource), (void**)&m_pSource));

done:
    // SAFE_RELEASE(pSourceResolver);
    // SAFE_RELEASE(pSource);
    return hr;
}

///////////////////////////////////////////////////////////////////////
//  CreateTopologyFromSource
//  Description:  Create a playback topology from the media source.
//
//  Pre-condition: The media source must be created already.
//                 Call CreateMediaSource() before calling this method.
/////////////////////////////////////////////////////////////////////////

HRESULT MFMediaPlayer::CreateTopologyFromSource(IMFTopology** ppTopology)
{
    TRACE((L"MFMediaPlayer::CreateTopologyFromSource\n"));

    assert(m_pSession != NULL);
    assert(m_pSource != NULL);

    HRESULT hr = S_OK;

    TComPtr<IMFTopology> pTopology;
    DWORD cSourceStreams = 0;

    // Create a new topology.
    CHECK_HR(hr = MFCreateTopology(&pTopology));

    // Create the presentation descriptor for the media source.
    CHECK_HR(hr = m_pSource->CreatePresentationDescriptor(&m_PresentDescriptor));

    // Get the number of streams in the media source.
    CHECK_HR(hr = m_PresentDescriptor->GetStreamDescriptorCount(&cSourceStreams));

    TRACE((L"Stream count: %d\n", cSourceStreams));

    // For each stream, create the topology nodes and add them to the topology.
    for (DWORD i = 0; i < cSourceStreams; ++i)
    {
        CHECK_HR(hr = AddBranchToPartialTopology(pTopology, m_PresentDescriptor, i));
    }

    // Return the IMFTopology pointer to the caller.
    if (SUCCEEDED(hr))
    {
        *ppTopology = pTopology;
        (*ppTopology)->AddRef();
    }

done:
    // SAFE_RELEASE(pTopology);
    // SAFE_RELEASE(pSourcePD);
    return hr;
}

///////////////////////////////////////////////////////////////////////
//  Name:  AddBranchToPartialTopology
//  Description:  Adds a topology branch for one stream.
//
//  pTopology: Pointer to the topology object.
//  pSourcePD: The source's presentation descriptor.
//  iStream: Index of the stream to render.
//
//  Pre-conditions: The topology must be created already.
//
//  Notes: For each stream, we must do the following:
//    1. Create a source node associated with the stream.
//    2. Create an output node for the renderer.
//    3. Connect the two nodes.
//  The media session will resolve the topology, so we do not have
//  to worry about decoders or other transforms.
/////////////////////////////////////////////////////////////////////////

HRESULT MFMediaPlayer::AddBranchToPartialTopology(IMFTopology* pTopology,
                                                   IMFPresentationDescriptor* pSourcePD,
                                                   DWORD iStream)
{
    TRACE((L"MFMediaPlayer::AddBranchToPartialTopology\n"));

    assert(pTopology != NULL);

    TComPtr<IMFStreamDescriptor> pSourceSD;
    TComPtr<IMFTopologyNode> pSourceNode;
    TComPtr<IMFTopologyNode> pOutputNode;
    BOOL fSelected = FALSE;

    HRESULT hr = S_OK;

    // Get the stream descriptor for this stream.
    CHECK_HR(hr = pSourcePD->GetStreamDescriptorByIndex(iStream, &fSelected, &pSourceSD));

    // Create the topology branch only if the stream is selected.
    // Otherwise, do nothing.
    if (fSelected)
    {
        // Create a source node for this stream.
        CHECK_HR(hr = CreateSourceStreamNode(m_pSource, pSourcePD, pSourceSD, &pSourceNode));

        // Create the output node for the renderer.
        CHECK_HR(hr = CreateOutputNode(pSourceSD, &pOutputNode));

        // Add both nodes to the topology.
        CHECK_HR(hr = pTopology->AddNode(pSourceNode));

        CHECK_HR(hr = pTopology->AddNode(pOutputNode));

        // Connect the source node to the output node.
        CHECK_HR(hr = pSourceNode->ConnectOutput(0, pOutputNode, 0));
    }

done:
    // Clean up.
    // SAFE_RELEASE(pSourceSD);
    // SAFE_RELEASE(pSourceNode);
    // SAFE_RELEASE(pOutputNode);
    return hr;
}

/// Static functions

//-------------------------------------------------------------------
//  Name: CreateSourceStreamNode
//  Description:  Creates a source-stream node for a stream.
//
//  pSource: Pointer to the media source that contains the stream.
//  pSourcePD: Presentation descriptor for the media source.
//  pSourceSD: Stream descriptor for the stream.
//  ppNode: Receives a pointer to the new node.
//-------------------------------------------------------------------

HRESULT CreateSourceStreamNode(IMFMediaSource* pSource,
                               IMFPresentationDescriptor* pSourcePD,
                               IMFStreamDescriptor* pSourceSD,
                               IMFTopologyNode** ppNode)
{
    if (!pSource || !pSourcePD || !pSourceSD || !ppNode)
    {
        return E_POINTER;
    }

    TComPtr<IMFTopologyNode> SourceNode;
    HRESULT hr = S_OK;

    // Create the source-stream node.
    CHECK_HR(hr = MFCreateTopologyNode(MF_TOPOLOGY_SOURCESTREAM_NODE, &SourceNode));

    // Set attribute: Pointer to the media source.
    CHECK_HR(hr = SourceNode->SetUnknown(MF_TOPONODE_SOURCE, pSource));

    // Set attribute: Pointer to the presentation descriptor.
    CHECK_HR(hr = SourceNode->SetUnknown(MF_TOPONODE_PRESENTATION_DESCRIPTOR, pSourcePD));

    // Set attribute: Pointer to the stream descriptor.
    CHECK_HR(hr = SourceNode->SetUnknown(MF_TOPONODE_STREAM_DESCRIPTOR, pSourceSD));

    // Return the IMFTopologyNode pointer to the caller.
    *ppNode = SourceNode;
    (*ppNode)->AddRef();

done:
    // SAFE_RELEASE(pNode);
    return hr;
}

/**
 * Implements a callback object for the WMF sample grabber sink.
 */
class FWmfMediaSampler : public IMFSampleGrabberSinkCallback
{
public:
    /** Default constructor. */
    FWmfMediaSampler() : RefCount(0) {}

public:
    /** Get an event that gets fired when the sampler's presentation clock changed its state. */
    // DECLARE_EVENT_OneParam(FWmfMediaSampler, FOnClock, EWmfMediaSamplerClockEvent /*Event*/)
    // FOnClock& OnClock()
    //{
    //    return ClockEvent;
    //}

    /** Get an event that gets fired when a new sample is ready (handler must be thread-safe). */
    // DECLARE_EVENT_FourParams(FWmfMediaSampler, FOnSample, const uint8* /*Buffer*/, uint32 /*Size*/, FTimespan
    // /*Duration*/, FTimespan /*Time*/)
    // FOnSample& OnSample()
    //{
    //    return SampleEvent;
    //}

public:
    //~ IMFSampleGrabberSinkCallback interface

    STDMETHODIMP OnClockPause(MFTIME SystemTime)
    {
        // ClockEvent.Broadcast(EWmfMediaSamplerClockEvent::Paused);

        return S_OK;
    }

    STDMETHODIMP OnClockRestart(MFTIME SystemTime)
    {
        // ClockEvent.Broadcast(EWmfMediaSamplerClockEvent::Restarted);

        return S_OK;
    }

    STDMETHODIMP OnClockSetRate(MFTIME SystemTime, float flRate) { return S_OK; }

    STDMETHODIMP OnClockStart(MFTIME SystemTime, LONGLONG llClockStartOffset)
    {
        // ClockEvent.Broadcast(EWmfMediaSamplerClockEvent::Started);

        return S_OK;
    }

    STDMETHODIMP OnClockStop(MFTIME SystemTime)
    {
        // ClockEvent.Broadcast(EWmfMediaSamplerClockEvent::Stopped);

        return S_OK;
    }

    STDMETHODIMP OnProcessSample(REFGUID MajorMediaType,
                                 DWORD SampleFlags,
                                 LONGLONG SampleTime,
                                 LONGLONG SampleDuration,
                                 const BYTE* SampleBuffer,
                                 DWORD SampleSize)
    {
        // SampleEvent.Broadcast((uint8*)SampleBuffer, (uint32)SampleSize, FTimespan(SampleDuration),
        // FTimespan(SampleTime));
        if (SampleEvent)
            SampleEvent((uint8_t*)SampleBuffer, SampleSize);
        return S_OK;
    }

    STDMETHODIMP OnSetPresentationClock(IMFPresentationClock* Clock) { return S_OK; }

    STDMETHODIMP OnShutdown() { return S_OK; }

public:
    //~ IUnknown interface

    STDMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&RefCount); }

#if _MSC_VER == 1900
#    pragma warning(push)
#    pragma warning(disable : 4838)
#endif

    STDMETHODIMP QueryInterface(REFIID RefID, void** Object)
    {
        static const QITAB QITab[] = {QITABENT(FWmfMediaSampler, IMFSampleGrabberSinkCallback), {0}};

        return QISearch(this, QITab, RefID, Object);
    }

#if _MSC_VER == 1900
#    pragma warning(pop)
#endif

    STDMETHODIMP_(ULONG) Release()
    {
        auto CurrentRefCount = InterlockedDecrement(&RefCount);

        if (CurrentRefCount == 0)
        {
            delete this;
        }

        return CurrentRefCount;
    }

private:
    /** Hidden destructor (this class is reference counted). */
    virtual ~FWmfMediaSampler() { assert(RefCount == 0); }

private:
    /** Event that gets fired when the sampler's presentation clock changed state. */
    // FOnClock ClockEvent;

    /** Holds a reference counter for this instance. */
    long RefCount;

    /** Event that gets fired when a new sample is ready. */
    // FOnSample SampleEvent;

public:
    SampleEventCallback SampleEvent;
};

bool IsFormatFamily(const GUID& lhs, const GUID& rhs)
{
    return memcmp(&lhs.Data2, &rhs.Data2, 12) == 0;  //-V512
}

HRESULT CopyAttribute(IMFAttributes* Src, IMFAttributes* Dest, const GUID& Key)
{
    PROPVARIANT var;
    PropVariantInit(&var);

    HRESULT Result = Src->GetItem(Key, &var);

    if (SUCCEEDED(Result))
    {
        Result = Dest->SetItem(Key, var);
        PropVariantClear(&var);
    }

    return Result;
}

static TComPtr<IMFMediaType> CreateOutputType(IMFMediaType& InputType,
                                       bool AllowNonStandardCodecs = true,
                                       bool IsVideoDevice          = false)
{
    GUID MajorType;
    {
        const HRESULT Result = InputType.GetGUID(MF_MT_MAJOR_TYPE, &MajorType);

        if (FAILED(Result))
        {
            TRACE(LogWmfMedia, Warning, ("Failed to get major type: %s"), ResultToString(Result));
            return NULL;
        }
    }

    GUID SubType;
    {
        const HRESULT Result = InputType.GetGUID(MF_MT_SUBTYPE, &SubType);

        if (FAILED(Result))
        {
            TRACE(LogWmfMedia, Warning, ("Failed to get sub-type: %s"), ResultToString(Result));
            return NULL;
        }
    }

    TComPtr<IMFMediaType> OutputType;
    {
        HRESULT Result = ::MFCreateMediaType(&OutputType);

        if (FAILED(Result))
        {
            TRACE(LogWmfMedia, Warning, ("Failed to create %s output type: %s"), MajorTypeToString(MajorType),
                   ResultToString(Result));
            return NULL;
        }

        Result = OutputType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);

        if (FAILED(Result))
        {
            TRACE(LogWmfMedia, Warning, ("Failed to initialize %s output type: %s"), MajorTypeToString(MajorType),
                   ResultToString(Result));
            return NULL;
        }
    }

    if (MajorType == MFMediaType_Audio)
    {
        // filter unsupported audio formats
        if (IsFormatFamily(SubType, MFMPEG4Format_Base))
        {
            if (AllowNonStandardCodecs)
            {
                TRACE(LogWmfMedia, Verbose, ("Allowing non-standard MP4 audio type %s (%s) \"%s\""),
                       SubTypeToString(SubType), *GuidToString(SubType), *FourccToString(SubType.Data1));
            }
            else
            {
                const bool DocumentedFormat =
                    (SubType.Data1 == WAVE_FORMAT_ADPCM) || (SubType.Data1 == WAVE_FORMAT_ALAW) ||
                    (SubType.Data1 == WAVE_FORMAT_MULAW) || (SubType.Data1 == WAVE_FORMAT_IMA_ADPCM) ||
                    (SubType.Data1 == MFAudioFormat_AAC.Data1) || (SubType.Data1 == MFAudioFormat_MP3.Data1) ||
                    (SubType.Data1 == MFAudioFormat_PCM.Data1);

                const bool UndocumentedFormat = (SubType.Data1 == WAVE_FORMAT_WMAUDIO2) ||
                                                (SubType.Data1 == WAVE_FORMAT_WMAUDIO3) ||
                                                (SubType.Data1 == WAVE_FORMAT_WMAUDIO_LOSSLESS);

                if (!DocumentedFormat && !UndocumentedFormat)
                {
                    TRACE(LogWmfMedia, Warning, ("Skipping non-standard MP4 audio type %s (%s) \"%s\""),
                           SubTypeToString(SubType), *GuidToString(SubType), *FourccToString(SubType.Data1));
                    return NULL;
                }
            }
        }
        else if (!IsFormatFamily(SubType, MFAudioFormat_Base))
        {
            if (AllowNonStandardCodecs)
            {
                TRACE(LogWmfMedia, Verbose, ("Allowing non-standard audio type %s (%s) \"%s\""),
                       SubTypeToString(SubType), *GuidToString(SubType), *FourccToString(SubType.Data1));
            }
            else
            {
                TRACE(LogWmfMedia, Warning, ("Skipping non-standard audio type %s (%s) \"%s\""),
                       SubTypeToString(SubType), *GuidToString(SubType), *FourccToString(SubType.Data1));
                return NULL;
            }
        }

        // configure audio output
        if (FAILED(OutputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio)) ||
            FAILED(OutputType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM)) ||
            FAILED(OutputType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16u)))
        {
            TRACE(LogWmfMedia, Warning, ("Failed to initialize audio output type"));
            return NULL;
        }

        // copy media type attributes
        if (FAILED(CopyAttribute(&InputType, OutputType, MF_MT_AUDIO_NUM_CHANNELS)) ||
            FAILED(CopyAttribute(&InputType, OutputType, MF_MT_AUDIO_SAMPLES_PER_SECOND)))
        {
            TRACE(LogWmfMedia, Warning, ("Failed to copy audio output type attributes"));
            return NULL;
        }
    }
    else if (MajorType == MFMediaType_Binary)
    {
        const HRESULT Result = OutputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Binary);

        if (FAILED(Result))
        {
            TRACE(LogWmfMedia, Warning, ("Failed to initialize binary output type: %s"), ResultToString(Result));
            return NULL;
        }
    }
    else if (MajorType == MFMediaType_SAMI)
    {
        // configure caption output
        const HRESULT Result = OutputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_SAMI);

        if (FAILED(Result))
        {
            TRACE(LogWmfMedia, Warning, TEXT("Failed to initialize caption output type: %s"), ResultToString(Result));
            return NULL;
        }
    }
    else if (MajorType == MFMediaType_Video)
    {
        // filter unsupported video types
        if (!IsFormatFamily(SubType, MFVideoFormat_Base))
        {
            if (AllowNonStandardCodecs)
            {
                TRACE(LogWmfMedia, Verbose, TEXT("Allowing non-standard video type %s (%s) \"%s\""),
                       SubTypeToString(SubType), *GuidToString(SubType), *FourccToString(SubType.Data1));
            }
            else
            {
                TRACE(LogWmfMedia, Warning, TEXT("Skipping non-standard video type %s (%s) \"%s\""),
                       SubTypeToString(SubType), *GuidToString(SubType), *FourccToString(SubType.Data1));
                return NULL;
            }
        }

        if ((SubType == MFVideoFormat_H264) || (SubType == MFVideoFormat_H264_ES))
        {
            if (IsVideoDevice)
            {
                TRACE(LogWmfMedia, Warning, TEXT("H264 video type requires Windows 8 or newer (your version is %s)"),
                       GetOSVersion());
                return NULL;
            }
        }

        //if ((SubType == MFVideoFormat_HEVC) || (SubType == MFVideoFormat_HEVC_ES))
        //{
        //}

        // configure video output
        HRESULT Result = OutputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);

        if (FAILED(Result))
        {
            TRACE(LogWmfMedia, Warning, TEXT("Failed to set video output type: %s"), ResultToString(Result));
            return NULL;
        }

        TRACE(LogWmfMedia, Verbose,
               TEXT("SubType: { 0x%08X, 0x%04X, 0x%04X, { 0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X, "
                    "0x%02X } };"),
               SubType.Data1, SubType.Data2, SubType.Data3, SubType.Data4[0], SubType.Data4[1], SubType.Data4[2],
               SubType.Data4[3], SubType.Data4[4], SubType.Data4[5], SubType.Data4[6], SubType.Data4[7]);

        if ((SubType == MFVideoFormat_HEVC) || (SubType == MFVideoFormat_HEVC_ES) || (SubType == MFVideoFormat_NV12) ||
            (SubType == MFVideoFormat_IYUV))
        {
            Result = OutputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        }
        else
        {
            const bool Uncompressed = (SubType == MFVideoFormat_RGB555) || (SubType == MFVideoFormat_RGB565) ||
                                      (SubType == MFVideoFormat_RGB24) || (SubType == MFVideoFormat_RGB32) ||
                                      (SubType == MFVideoFormat_ARGB32);

            Result = OutputType->SetGUID(
                MF_MT_SUBTYPE, Uncompressed ? MFVideoFormat_RGB32 : MFVideoFormat_NV12 /*MFVideoFormat_YUY2*/);
        }

        if (FAILED(Result))
        {
            TRACE(LogWmfMedia, Warning, TEXT("Failed to set video output sub-type: %s"), ResultToString(Result));
            return NULL;
        }

        // copy media type attributes
        if (IsVideoDevice)
        {
            // the following attributes seem to help with web cam issues on Windows 7,
            // but we generally don't want to copy these for any other media sources
            // and let the WMF topology resolver pick optimal defaults instead.

            if (FAILED(CopyAttribute(&InputType, OutputType, MF_MT_FRAME_RATE)) ||
                FAILED(CopyAttribute(&InputType, OutputType, MF_MT_FRAME_SIZE)))
            {
                TRACE(LogWmfMedia, Warning, TEXT("Failed to copy video output type attributes"));
                return NULL;
            }
        }
    }
    else
    {
        return NULL;  // unsupported input type
    }

    return OutputType;
}

//-------------------------------------------------------------------
//  Name: CreateOutputNode
//  Description:  Create an output node for a stream.
//
//  pSourceSD: Stream descriptor for the stream.
//  ppNode: Receives a pointer to the new node.
//
//  Notes:
//  This function does the following:
//  1. Chooses a renderer based on the media type of the stream.
//  2. Creates an IActivate object for the renderer.
//  3. Creates an output topology node.
//  4. Sets the IActivate pointer on the node.
//-------------------------------------------------------------------

HRESULT MFMediaPlayer::CreateOutputNode(IMFStreamDescriptor* pSourceSD, IMFTopologyNode** ppNode)
{

    TComPtr<IMFTopologyNode> OutputNode;
    TComPtr<IMFMediaTypeHandler> pHandler;
    TComPtr<IMFActivate> pRendererActivate;

    GUID guidMajorType = GUID_NULL;
    HRESULT hr         = S_OK;

    // Get the stream ID.
    DWORD streamID = 0;
    pSourceSD->GetStreamIdentifier(&streamID);  // Just for debugging, ignore any failures.

    // Get the media type handler for the stream.
    CHECK_HR(hr = pSourceSD->GetMediaTypeHandler(&pHandler));

    // Get the major media type.
    CHECK_HR(hr = pHandler->GetMajorType(&guidMajorType));

    // Create a downstream node.
    CHECK_HR(hr = MFCreateTopologyNode(MF_TOPOLOGY_OUTPUT_NODE, &OutputNode));

    // Create an IMFActivate object for the renderer, based on the media type.
    if (MFMediaType_Video == guidMajorType)
    {
        // Create the video renderer.
        TRACE((L"Stream %d: video stream\n", streamID));
        // CHECK_HR(hr = MFCreateVideoRendererActivate(hwndVideo, &pRendererActivate));
        TComPtr<FWmfMediaSampler> Sampler = new FWmfMediaSampler();
        TComPtr<IMFMediaType> InputType;
        hr = pHandler->GetCurrentMediaType(&InputType);

        // GUID SubType;
        // InputType->GetGUID(MF_MT_SUBTYPE, &SubType);
        // auto strSubType = SubTypeToString(SubType);

        // UINT32 Width = 0, Height = 0;
        hr = MFGetAttributeSize(InputType, MF_MT_FRAME_SIZE, &m_uVideoWidth, &m_uVideoHeight);

        auto OutputType = CreateOutputType(*InputType);

        hr = ::MFCreateSampleGrabberSinkActivate(OutputType, Sampler, &pRendererActivate);
        CHECK_HR(hr);

        Sampler->SampleEvent = this->SampleEvent;

        // To run as fast as possible, set this attribute (requires Windows 7):
        // CHECK_HR(hr = pRendererActivate->SetUINT32(MF_SAMPLEGRABBERSINK_IGNORE_CLOCK, TRUE));
    }
    else if (MFMediaType_Audio == guidMajorType)
    {
        // Create the audio renderer.
        TRACE((L"Stream %d: audio stream\n", streamID));
        CHECK_HR(hr = MFCreateAudioRendererActivate(&pRendererActivate));
    }
    else
    {
        TRACE((L"Stream %d: Unknown format\n", streamID));
        CHECK_HR(hr = E_FAIL);
    }

    // Set the IActivate object on the output node.
    CHECK_HR(hr = OutputNode->SetObject(pRendererActivate));
    CHECK_HR(hr = OutputNode->SetUINT32(MF_TOPONODE_STREAMID, 0));

    // Return the IMFTopologyNode pointer to the caller.
    *ppNode = OutputNode;
    (*ppNode)->AddRef();

done:
    return hr;
}
