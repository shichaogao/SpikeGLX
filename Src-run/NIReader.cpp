
#include "NIReader.h"
#include "Util.h"
#include "CniAcqDmx.h"
#include "CniAcqSim.h"

#include <QThread>


/* ---------------------------------------------------------------- */
/* NIReaderWorker ------------------------------------------------- */
/* ---------------------------------------------------------------- */

NIReaderWorker::NIReaderWorker( const Params &p, AIQ *niQ )
    :   QObject(0), niQ(niQ)
{
#ifdef HAVE_NIDAQmx
    niAcq = new CniAcqDmx( this, p );
#else
    niAcq = new CniAcqSim( this, p );
#endif
}


NIReaderWorker::~NIReaderWorker()
{
    delete niAcq;
}


void NIReaderWorker::stop()
{
    niAcq->stop();
}


void NIReaderWorker::run()
{
    niAcq->run();

    emit finished();
}

/* ---------------------------------------------------------------- */
/* NIReader ------------------------------------------------------- */
/* ---------------------------------------------------------------- */

NIReader::NIReader( const Params &p, AIQ *niQ )
{
    thread  = new QThread;
    worker  = new NIReaderWorker( p, niQ );

    worker->moveToThread( thread );

    Connect( thread, SIGNAL(started()), worker, SLOT(run()) );
    Connect( worker, SIGNAL(finished()), worker, SLOT(deleteLater()) );
    Connect( worker, SIGNAL(destroyed()), thread, SLOT(quit()), Qt::DirectConnection );

// Thread manually started by run manager.
//    thread->start();
}


NIReader::~NIReader()
{
// worker object auto-deleted asynchronously
// thread object manually deleted synchronously (so we can call wait())

    if( thread->isRunning() ) {

        worker->stop();
        thread->wait();
    }

    delete thread;
}


void NIReader::start()
{
    thread->start();
}

