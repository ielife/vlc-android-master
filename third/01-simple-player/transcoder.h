#ifndef TRANSCODER_H
#define TRANSCODER_H

#include <string>
extern "C"
{
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavfilter/avfiltergraph.h"
#include "libavcodec/avcodec.h"
#include "libavfilter/buffersink.h"
#include "libavfilter/buffersrc.h"
#include "libavutil/avutil.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
};

class Transcoder {
public:
    Transcoder():mIfmtCtx(0),mOfmtCtx(0),mFilterCtx(0),mpFrame(0)
    {isInitSuccess = false;}
    ~Transcoder(){}

    class FilteringContext {
    public:
        AVFilterContext *buffersink_ctx;
        AVFilterContext *buffersrc_ctx;
        AVFilterGraph   *filter_graph;
    } ;

    bool CreateRes(const char *psrcFile, const char *pDstFile);
    bool Start();
    void DestoryRes();
protected:
    int  OpenInputFile(const char *pathname);
    int  OpenOutputFile(const char *pathname);
    int  InitFilter(FilteringContext* fctx, AVCodecContext *dec_ctx,
                    AVCodecContext *enc_ctx, const char *filter_spec);
    int  InitFilters();
    int  EncodeWriteFrame(AVFrame *filt_frame, unsigned int stream_index, int *got_frame);
    int  FilterEncodeWriteFrame(AVFrame *frame, unsigned int stream_index);
    int  FlushEncoder(unsigned int stream_index);

private:

    bool                isInitSuccess;

    std::string         mInputFile;
    std::string         mOutputFile;

    AVFormatContext     *mIfmtCtx;
    AVFormatContext     *mOfmtCtx;
    FilteringContext    *mFilterCtx;

    AVPacket            mPacket;
    AVFrame             *mpFrame;
};

#endif // TRANSCODER_H
