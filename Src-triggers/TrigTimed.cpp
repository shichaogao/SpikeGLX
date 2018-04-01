
#include "TrigTimed.h"
#include "Util.h"
#include "MainApp.h"
#include "Run.h"

#include <QThread>


#define LOOP_MS     100


static TrigTimed    *ME;


/* ---------------------------------------------------------------- */
/* TrTimWorker ---------------------------------------------------- */
/* ---------------------------------------------------------------- */

void TrTimWorker::run()
{
    const int   nID = vID.size();
    bool        ok  = true;

    for(;;) {

        if( !shr.wake( ok ) )
            break;

        for( int iID = 0; iID < nID; ++iID ) {

            if( !(ok = doSomeHIm( vID[iID] )) )
                break;
        }
    }

    emit finished();
}


// Return true if no errors.
//
bool TrTimWorker::doSomeHIm( int ip )
{
    TrigTimed::CountsIm &C      = ME->imCnt;
    vec_i16             data;
    quint64             headCt  = C.nextCt[ip];
    uint                remCt   = C.hiCtMax[ip] - C.hiCtCur[ip],
                        nMax    = (remCt <= C.maxFetch[ip] ?
                                    remCt : C.maxFetch[ip]);

    try {
        data.reserve( imQ[ip]->nChans() * nMax );
    }
    catch( const std::exception& ) {
        Error() << "Trigger low mem";
        return false;
    }

    if( !imQ[ip]->getNScansFromCt( data, headCt, nMax ) )
        return false;

    uint    size = data.size();

    if( !size )
        return true;

// ---------------
// Update tracking
// ---------------

    C.nextCt[ip]    += size / imQ[ip]->nChans();
    C.hiCtCur[ip]   += C.nextCt[ip] - headCt;

// -----
// Write
// -----

    return ME->writeAndInvalData( ME->DstImec, ip, data, headCt );
}

/* ---------------------------------------------------------------- */
/* TrTimThread ---------------------------------------------------- */
/* ---------------------------------------------------------------- */

TrTimThread::TrTimThread(
    TrTimShared         &shr,
    const QVector<AIQ*> &imQ,
    QVector<int>        &vID )
{
    thread  = new QThread;
    worker  = new TrTimWorker( shr, imQ, vID );

    worker->moveToThread( thread );

    Connect( thread, SIGNAL(started()), worker, SLOT(run()) );
    Connect( worker, SIGNAL(finished()), worker, SLOT(deleteLater()) );
    Connect( worker, SIGNAL(destroyed()), thread, SLOT(quit()), Qt::DirectConnection );

    thread->start();
}


TrTimThread::~TrTimThread()
{
// worker object auto-deleted asynchronously
// thread object manually deleted synchronously (so we can call wait())

    if( thread->isRunning() )
        thread->wait();

    delete thread;
}

/* ---------------------------------------------------------------- */
/* CountsIm ------------------------------------------------------- */
/* ---------------------------------------------------------------- */

TrigTimed::CountsIm::CountsIm( const DAQ::Params &p )
    :   np(p.im.nProbes)
{
    hiCtMax.resize( np );
    loCt.resize( np );
    maxFetch.resize( np );

    for( int ip = 0; ip < np; ++ip ) {

        double  srate = p.im.each[ip].srate;

        hiCtMax[ip] =
            (p.trgTim.isHInf ?
            std::numeric_limits<qlonglong>::max()
            : p.trgTim.tH * srate);

        loCt[ip]        = p.trgTim.tL * srate;
        maxFetch[ip]    = 0.110 * srate;
    }
}


bool TrigTimed::CountsIm::hDone()
{
    for( int ip = 0; ip < np; ++ip ) {

        if( hiCtCur[ip] < hiCtMax[ip] )
            return false;
    }

    return true;
}


void TrigTimed::CountsIm::hNext()
{
    for( int ip = 0; ip < np; ++ip )
        nextCt[ip] += loCt[ip];
}

/* ---------------------------------------------------------------- */
/* CountsNi ------------------------------------------------------- */
/* ---------------------------------------------------------------- */

TrigTimed::CountsNi::CountsNi( const DAQ::Params &p )
    :   nextCt(0), hiCtCur(0),
        hiCtMax(
            p.trgTim.isHInf ?
            std::numeric_limits<qlonglong>::max()
            : p.trgTim.tH * p.ni.srate),
        loCt(p.trgTim.tL * p.ni.srate),
        maxFetch(0.110 * p.ni.srate),
        enabled(p.ni.enabled)
{
}


bool TrigTimed::CountsNi::hDone()
{
    return !enabled || hiCtCur >= hiCtMax;
}


void TrigTimed::CountsNi::hNext()
{
     nextCt += loCt;
}

/* ---------------------------------------------------------------- */
/* TrigTimed ------------------------------------------------------ */
/* ---------------------------------------------------------------- */

TrigTimed::TrigTimed(
    const DAQ::Params   &p,
    GraphsWindow        *gw,
    const QVector<AIQ*> &imQ,
    const AIQ           *niQ )
    :   TrigBase( p, gw, imQ, niQ ),
        imCnt( p ),
        niCnt( p ),
        nCycMax(
            p.trgTim.isNInf ?
            std::numeric_limits<qlonglong>::max()
            : p.trgTim.nH)
{
}


void TrigTimed::setGate( bool hi )
{
    runMtx.lock();
    initState();
    baseSetGate( hi );
    runMtx.unlock();
}


void TrigTimed::resetGTCounters()
{
    runMtx.lock();
    baseResetGTCounters();
    initState();
    runMtx.unlock();
}


#define SETSTATE_L0     (state = 0)
#define SETSTATE_H      (state = 1)
#define SETSTATE_L      (state = 2)

#define ISSTATE_L0      (state == 0)
#define ISSTATE_H       (state == 1)
#define ISSTATE_L       (state == 2)
#define ISSTATE_Done    (state == 3)


// Timed logic is driven by TrgTimParams: {tL0, tH, tL, nH}.
// There are four corresponding states defined above.
//
void TrigTimed::run()
{
    Debug() << "Trigger thread started.";

// ---------
// Configure
// ---------

    ME = this;

// Create worker threads

    const int               nPrbPerThd = 2;

    QVector<TrTimThread*>   trT;
    TrTimShared             shr( p );

    nThd = 0;

    for( int ip0 = 0; ip0 < nImQ; ip0 += nPrbPerThd ) {

        QVector<int>    vID;

        for( int id = 0; id < nPrbPerThd; ++id ) {

            if( ip0 + id < nImQ )
                vID.push_back( ip0 + id );
            else
                break;
        }

        trT.push_back( new TrTimThread( shr, imQ, vID ) );
        ++nThd;
    }

// Wait for threads to reach ready (sleep) state

    shr.runMtx.lock();
        while( shr.asleep < nThd ) {
            shr.runMtx.unlock();
                QThread::usleep( 10 );
            shr.runMtx.lock();
        }
    shr.runMtx.unlock();

// -----
// Start
// -----

    setYieldPeriod_ms( LOOP_MS );

    initState();

    QString err;

    while( !isStopped() ) {

        double  loopT   = getTime(),
                gHiT,
                delT    = 0;
        bool    inactive;

        // -------
        // Active?
        // -------

        inactive = ISSTATE_Done || !isGateHi();

        if( inactive ) {

            endTrig();
            goto next_loop;
        }

        // ------------------
        // Current gate start
        // ------------------

        gHiT = getGateHiT();

        // ----------------------------
        // L0 phase (waiting for start)
        // ----------------------------

        if( ISSTATE_L0 )
            delT = remainingL0( loopT, gHiT );

        // -----------------
        // H phase (writing)
        // -----------------

        if( ISSTATE_H ) {

            if( !allDoSomeH( shr, gHiT ) ) {
                err = "Generic error";
                break;
            }

            // Done?

            if( niCnt.hDone() && imCnt.hDone() ) {

                if( ++nH >= nCycMax ) {
                    SETSTATE_Done();
                    inactive = true;
                }
                else {
                    alignNextFiles();
                    SETSTATE_L;
                }

                endTrig();
            }
        }

        // ----------------------------
        // L phase (waiting for next H)
        // ----------------------------

        if( ISSTATE_L ) {

            if( niQ )
                delT = remainingL( niQ, niCnt.nextCt );
            else
                delT = remainingL( imQ[0], imCnt.nextCt[0] );
        }

        // ------
        // Status
        // ------

next_loop:
        if( loopT - statusT > 0.25 ) {

            QString sOn, sT, sWr;
            int     ig, it;

            getGT( ig, it );
            statusOnSince( sOn, ig, it );
            statusWrPerf( sWr );

            if( inactive )
                sT = " TX";
            else if( ISSTATE_L0 || ISSTATE_L )
                sT = QString(" T-%1s").arg( delT, 0, 'f', 1 );
            else {

                double  hisec;

                if( niQ )
                    hisec = niCnt.hiCtCur / p.ni.srate;
                else    // any representative probe OK
                    hisec = imCnt.hiCtCur[0] / p.im.each[0].srate;

                sT = QString(" T+%1s").arg( hisec, 0, 'f', 1 );
            }

            Status() << sOn << sT << sWr;

            statusT = loopT;
        }

        // -------------------
        // Moderate fetch rate
        // -------------------

        yield( loopT );
    }

// Kill all threads

    shr.kill();

    for( int iThd = 0; iThd < nThd; ++iThd )
        delete trT[iThd];

// Done

    endRun( err );
}


void TrigTimed::SETSTATE_Done()
{
    state = 3;
    mainApp()->getRun()->dfSetRecordingEnabled( false, true );
}


void TrigTimed::initState()
{
    nH = 0;

    imCnt.nextCt.clear();
    niCnt.nextCt = 0;

    if( p.trgTim.tL0 > 0 )
        SETSTATE_L0;
    else
        SETSTATE_H;
}


// Return time remaining in L0 phase.
//
double TrigTimed::remainingL0( double loopT, double gHiT )
{
    double  elapsedT = loopT - gHiT;

    if( elapsedT < p.trgTim.tL0 )
        return p.trgTim.tL0 - elapsedT;  // remainder

    SETSTATE_H;

    return 0;
}


// Return time remaining in L phase.
//
double TrigTimed::remainingL( const AIQ *aiQ, quint64 nextCt )
{
    quint64 endCt = aiQ->endCount();

    if( endCt < nextCt )
        return (nextCt - endCt) / aiQ->sRate();

    SETSTATE_H;

    return 0;
}


// One-time concurrent setting of tracking data.
// mapTime2Ct may return false if the sought time mark
// isn't in the stream. The most likely failure mode is
// that the target time is too new, which is fixed by
// retrying on another loop iteration.
//
bool TrigTimed::alignFirstFiles( double gHiT )
{
    if( (nImQ && !imCnt.nextCt.size()) || (niQ && !niCnt.nextCt) ) {

        double              startT  = gHiT + p.trgTim.tL0;
        int                 ns      = vS.size(),
                            offset  = 0;
        QVector<quint64>    nextCt( ns );

        for( int is = 0; is < ns; ++is ) {
            if( 0 != vS[is].Q->mapTime2Ct( nextCt[is], startT ) )
                return false;
        }

        // set everybody's tAbs
        syncDstTAbsMult( nextCt[0], 0, vS, p );

        if( niQ ) {
           niCnt.nextCt = nextCt[0];
           offset       = 1;
        }

        if( nImQ ) {

            imCnt.nextCt.resize( nImQ );

            for( int ip = 0; ip < nImQ; ++ip ) {
                const SyncStream    &S = vS[offset+ip];
                imCnt.nextCt[ip] = S.TAbs2Ct( S.tAbs );
            }
        }
    }

    return true;
}


void TrigTimed::alignNextFiles()
{
    imCnt.hNext();
    niCnt.hNext();
}


// Return true if no errors.
//
bool TrigTimed::doSomeHNi()
{
    if( !niQ )
        return true;

    CountsNi    &C      = niCnt;
    vec_i16     data;
    quint64     headCt  = C.nextCt;
    uint        remCt   = C.hiCtMax - C.hiCtCur,
                nMax    = (remCt <= C.maxFetch ? remCt : C.maxFetch);

    try {
        data.reserve( niQ->nChans() * nMax );
    }
    catch( const std::exception& ) {
        Error() << "Trigger low mem";
        return false;
    }

    if( !niQ->getNScansFromCt( data, headCt, nMax ) )
        return false;

    uint    size = data.size();

    if( !size )
        return true;

// ---------------
// Update tracking
// ---------------

    C.nextCt    += size / niQ->nChans();
    C.hiCtCur   += C.nextCt - headCt;

// -----
// Write
// -----

    return writeAndInvalData( DstNidq, 0, data, headCt );
}


// Return true if no errors.
//
bool TrigTimed::xferAll( TrTimShared &shr )
{
    bool    niOK;

    shr.awake   = 0;
    shr.asleep  = 0;
    shr.errors  = 0;

// Wake all imec threads

    shr.condWake.wakeAll();

// Do nidq locally

    niOK = doSomeHNi();

// Wait all threads started, and all done

    shr.runMtx.lock();
        while( shr.awake  < nThd
            || shr.asleep < nThd ) {

            shr.runMtx.unlock();
                QThread::msleep( LOOP_MS/8 );
            shr.runMtx.lock();
        }
    shr.runMtx.unlock();

    return niOK && !shr.errors;
}


// Return true if no errors.
//
bool TrigTimed::allDoSomeH( TrTimShared &shr, double gHiT )
{
// -------------------
// Open files together
// -------------------

    if( allFilesClosed() ) {

        int ig, it;

        // reset tracking
        imCnt.hiCtCur.fill( 0, nImQ );
        niCnt.hiCtCur = 0;

        if( !newTrig( ig, it ) )
            return false;
    }

// ---------------------
// Seek common sync time
// ---------------------

    if( !alignFirstFiles( gHiT ) )
        return true;    // too early

// ----------------------
// Fetch from all streams
// ----------------------

    return xferAll( shr );
}


