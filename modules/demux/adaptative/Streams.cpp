/*
 * Streams.cpp
 *****************************************************************************
 * Copyright (C) 2014 - VideoLAN authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/
#define __STDC_CONSTANT_MACROS
#include "Streams.hpp"
#include "StreamsType.hpp"
#include "http/HTTPConnection.h"
#include "http/HTTPConnectionManager.h"
#include "http/Chunk.h"
#include "logic/AbstractAdaptationLogic.h"
#include "SegmentTracker.hpp"
#include <vlc_stream.h>
#include <vlc_demux.h>

using namespace adaptative::Streams;
using namespace adaptative::http;
using namespace adaptative::logic;

using namespace adaptative::Streams;

Stream::Stream(const std::string &mime)
{
    init(mimeToType(mime), mimeToFormat(mime));
}

Stream::Stream(const Type type, const Format format)
{
    init(type, format);
}

void Stream::init(const Type type_, const Format format_)
{
    type = type_;
    format = format_;
    output = NULL;
    adaptationLogic = NULL;
    currentChunk = NULL;
    eof = false;
    segmentTracker = NULL;
}

Stream::~Stream()
{
    delete currentChunk;
    delete adaptationLogic;
    delete output;
    delete segmentTracker;
}

Type Stream::mimeToType(const std::string &mime)
{
    Type mimetype;
    if (!mime.compare(0, 6, "video/"))
        mimetype = Streams::VIDEO;
    else if (!mime.compare(0, 6, "audio/"))
        mimetype = Streams::AUDIO;
    else if (!mime.compare(0, 12, "application/"))
        mimetype = Streams::APPLICATION;
    else /* unknown of unsupported */
        mimetype = Streams::UNKNOWN;
    return mimetype;
}

Format Stream::mimeToFormat(const std::string &mime)
{
    Format format = Streams::UNSUPPORTED;
    std::string::size_type pos = mime.find("/");
    if(pos != std::string::npos)
    {
        std::string tail = mime.substr(pos + 1);
        if(tail == "mp4")
            format = Streams::MP4;
        else if (tail == "mp2t")
            format = Streams::MPEG2TS;
    }
    return format;
}

void Stream::create(demux_t *demux, AbstractAdaptationLogic *logic, SegmentTracker *tracker)
{
    switch(format)
    {
        case adaptative::Streams::MP4:
            output = new MP4StreamOutput(demux);
            break;
        case adaptative::Streams::MPEG2TS:
            output = new MPEG2TSStreamOutput(demux);
            break;
        default:
            throw VLC_EBADVAR;
            break;
    }
    adaptationLogic = logic;
    segmentTracker = tracker;
}

bool Stream::isEOF() const
{
    return false;
}

mtime_t Stream::getPCR() const
{
    return output->getPCR();
}

int Stream::getGroup() const
{
    return output->getGroup();
}

int Stream::esCount() const
{
    return output->esCount();
}

bool Stream::operator ==(const Stream &stream) const
{
    return stream.type == type;
}

Chunk * Stream::getChunk()
{
    if (currentChunk == NULL)
    {
        currentChunk = segmentTracker->getNextChunk(type);
        if (currentChunk == NULL)
            eof = true;
    }
    return currentChunk;
}

bool Stream::seekAble() const
{
    return (output && output->seekAble());
}

size_t Stream::read(HTTPConnectionManager *connManager)
{
    Chunk *chunk = getChunk();
    if(!chunk)
        return 0;

    if(!chunk->getConnection())
    {
       if(!connManager->connectChunk(chunk))
        return 0;
    }

    size_t readsize = 0;

    /* Because we don't know Chunk size at start, we need to get size
       from content length */
    if(chunk->getBytesRead() == 0)
    {
        if(chunk->getConnection()->query(chunk->getPath()) == false)
            readsize = 32768; /* we don't handle retry here :/ */
        else
            readsize = chunk->getBytesToRead();
    }
    else
    {
        readsize = chunk->getBytesToRead();
    }

    if (readsize > 128000)
        readsize = 32768;

    block_t *block = block_Alloc(readsize);
    if(!block)
        return 0;

    mtime_t time = mdate();
    ssize_t ret = chunk->getConnection()->read(block->p_buffer, readsize);
    time = mdate() - time;

    if(ret <= 0)
    {
        block_Release(block);
        chunk->getConnection()->releaseChunk();
        currentChunk = NULL;
        delete chunk;
        return 0;
    }
    else
    {
        block->i_buffer = (size_t)ret;

        adaptationLogic->updateDownloadRate(block->i_buffer, time);

        if (chunk->getBytesToRead() == 0)
        {
            chunk->onDownload(block->p_buffer, block->i_buffer);
            chunk->getConnection()->releaseChunk();
            currentChunk = NULL;
            delete chunk;
        }
    }

    readsize = block->i_buffer;

    output->pushBlock(block);
    output->sendToDecoder(INT64_MAX - VLC_TS_0);

    return readsize;
}

bool Stream::setPosition(mtime_t time, bool tryonly)
{
    bool ret = segmentTracker->setPosition(time, tryonly);
    if(!tryonly && ret)
        output->setPosition(time);
    return ret;
}

mtime_t Stream::getPosition() const
{
    return segmentTracker->getSegmentStart();
}

AbstractStreamOutput::AbstractStreamOutput(demux_t *demux)
{
    realdemux = demux;
    demuxstream = NULL;
    pcr = VLC_TS_0;
    group = -1;
    seekable = true;

    fakeesout = new es_out_t;
    if (!fakeesout)
        throw VLC_ENOMEM;

    fakeesout->pf_add = esOutAdd;
    fakeesout->pf_control = esOutControl;
    fakeesout->pf_del = esOutDel;
    fakeesout->pf_destroy = esOutDestroy;
    fakeesout->pf_send = esOutSend;
    fakeesout->p_sys = (es_out_sys_t*) this;
}

AbstractStreamOutput::~AbstractStreamOutput()
{
    if (demuxstream)
        stream_Delete(demuxstream);
    delete fakeesout;
}

mtime_t AbstractStreamOutput::getPCR() const
{
    return pcr;
}

int AbstractStreamOutput::getGroup() const
{
    return group;
}

int AbstractStreamOutput::esCount() const
{
    return queues.size();
}

void AbstractStreamOutput::pushBlock(block_t *block)
{
    stream_DemuxSend(demuxstream, block);
}

bool AbstractStreamOutput::seekAble() const
{
    return (demuxstream && seekable);
}

void AbstractStreamOutput::setPosition(mtime_t nztime)
{
    std::list<Demuxed *>::const_iterator it;
    for(it=queues.begin(); it!=queues.end();++it)
    {
        Demuxed *pair = *it;
        if(pair->p_queue && pair->p_queue->i_dts > VLC_TS_0 + nztime)
            pair->drop();
    }
    pcr = VLC_TS_0;
    es_out_Control(realdemux->out, ES_OUT_SET_NEXT_DISPLAY_TIME,
                   VLC_TS_0 + nztime);
}

void AbstractStreamOutput::sendToDecoder(mtime_t nzdeadline)
{
    std::list<Demuxed *>::const_iterator it;
    for(it=queues.begin(); it!=queues.end();++it)
    {
        Demuxed *pair = *it;
        while(pair->p_queue && pair->p_queue->i_dts <= VLC_TS_0 + nzdeadline)
        {
            block_t *p_block = pair->p_queue;
            pair->p_queue = pair->p_queue->p_next;
            p_block->p_next = NULL;

            if(pair->pp_queue_last == &p_block->p_next)
                pair->pp_queue_last = &pair->p_queue;

            realdemux->out->pf_send(realdemux->out, pair->es_id, p_block);
        }
    }
}

void AbstractStreamOutput::dropQueues()
{
    std::list<Demuxed *>::const_iterator it;
    for(it=queues.begin(); it!=queues.end();++it)
        (*it)->drop();
}

AbstractStreamOutput::Demuxed::Demuxed()
{
    p_queue = NULL;
    pp_queue_last = &p_queue;
    es_id = NULL;
}

AbstractStreamOutput::Demuxed::~Demuxed()
{
    drop();
}

void AbstractStreamOutput::Demuxed::drop()
{
    block_ChainRelease(p_queue);
    p_queue = NULL;
    pp_queue_last = &p_queue;
}

/* Static callbacks */
es_out_id_t * AbstractStreamOutput::esOutAdd(es_out_t *fakees, const es_format_t *p_fmt)
{
    AbstractStreamOutput *me = (AbstractStreamOutput *) fakees->p_sys;
    es_out_id_t *p_es = me->realdemux->out->pf_add(me->realdemux->out, p_fmt);
    if(p_es)
    {
        Demuxed *pair = new (std::nothrow) Demuxed();
        if(pair)
        {
            pair->es_id = p_es;
            me->queues.push_back(pair);
        }
    }
    return p_es;
}

int AbstractStreamOutput::esOutSend(es_out_t *fakees, es_out_id_t *p_es, block_t *p_block)
{
    AbstractStreamOutput *me = (AbstractStreamOutput *) fakees->p_sys;
    std::list<Demuxed *>::const_iterator it;
    for(it=me->queues.begin(); it!=me->queues.end();++it)
    {
        Demuxed *pair = *it;
        if(pair->es_id == p_es)
        {
            block_ChainLastAppend(&pair->pp_queue_last, p_block);
            break;
        }
    }
    return VLC_SUCCESS;
}

void AbstractStreamOutput::esOutDel(es_out_t *fakees, es_out_id_t *p_es)
{
    AbstractStreamOutput *me = (AbstractStreamOutput *) fakees->p_sys;
    std::list<Demuxed *>::iterator it;
    for(it=me->queues.begin(); it!=me->queues.end();++it)
    {
        if((*it)->es_id == p_es)
        {
            delete *it;
            me->queues.erase(it);
            break;
        }
    }
    me->realdemux->out->pf_del(me->realdemux->out, p_es);
}

int AbstractStreamOutput::esOutControl(es_out_t *fakees, int i_query, va_list args)
{
    AbstractStreamOutput *me = (AbstractStreamOutput *) fakees->p_sys;
    if (i_query == ES_OUT_SET_PCR )
    {
        me->pcr = (int64_t)va_arg( args, int64_t );
        return VLC_SUCCESS;
    }
    else if( i_query == ES_OUT_SET_GROUP_PCR )
    {
        me->group = (int) va_arg( args, int );
        me->pcr = (int64_t)va_arg( args, int64_t );
        return VLC_SUCCESS;
    }

    return me->realdemux->out->pf_control(me->realdemux->out, i_query, args);
}

void AbstractStreamOutput::esOutDestroy(es_out_t *fakees)
{
    AbstractStreamOutput *me = (AbstractStreamOutput *) fakees->p_sys;
    me->realdemux->out->pf_destroy(me->realdemux->out);
}
/* !Static callbacks */

MP4StreamOutput::MP4StreamOutput(demux_t *demux) :
    AbstractStreamOutput(demux)
{
    demuxstream = stream_DemuxNew(demux, "mp4", fakeesout);
    if(!demuxstream)
        throw VLC_EGENERIC;
}

MPEG2TSStreamOutput::MPEG2TSStreamOutput(demux_t *demux) :
    AbstractStreamOutput(demux)
{
    demuxstream = stream_DemuxNew(demux, "ts", fakeesout);
    if(!demuxstream)
        throw VLC_EGENERIC;
}
