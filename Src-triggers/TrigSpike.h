#ifndef TRIGSPIKE_H
#define TRIGSPIKE_H

#include "TrigBase.h"

class Biquad;

/* ---------------------------------------------------------------- */
/* Types ---------------------------------------------------------- */
/* ---------------------------------------------------------------- */

class TrigSpike : public TrigBase
{
private:
    struct HiPassFnctr : public AIQ::T_AIQBlockFilter {
        Biquad  *flt;
        int     nchans,
                ichan,
                maxInt,
                nzero;
        HiPassFnctr( const DAQ::Params &p );
        virtual ~HiPassFnctr();
        void reset();
        void operator()( vec_i16 &data );
    };

    struct Counts {
        const quint64   periEvtCt,
                        refracCt,
                        latencyCt;
        quint64         edgeCt,
                        nextCt;
        qint64          remCt;

        Counts( const DAQ::Params &p, double srate )
        :   periEvtCt(p.trgSpike.periEvtSecs * srate),
            refracCt(std::max( p.trgSpike.refractSecs * srate, 5.0 )),
            latencyCt(0.25 * srate),
            edgeCt(0),
            nextCt(0),
            remCt(0)    {}
    };

private:
    HiPassFnctr     *usrFlt;
    Counts          imCnt,
                    niCnt;
    const qint64    nCycMax;
    int             nS,
                    state;

public:
    TrigSpike(
        DAQ::Params     &p,
        GraphsWindow    *gw,
        const AIQ       *imQ,
        const AIQ       *niQ );
    virtual ~TrigSpike()    {delete usrFlt;}

    virtual void setGate( bool hi );
    virtual void resetGTCounters();

public slots:
    virtual void run();

private:
    void initState();
    bool getEdgeIM();
    bool getEdgeNI();
    bool writeSome(
        DataFile    *df,
        const AIQ   *aiQ,
        Counts      &cnt );
};

#endif  // TRIGSPIKE_H

