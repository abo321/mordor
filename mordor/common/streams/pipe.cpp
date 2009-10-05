// Copyright (c) 2009 - Decho Corp.

#include "mordor/common/pch.h"

#include "pipe.h"

#include <boost/thread/mutex.hpp>

#include "mordor/common/exception.h"
#include "mordor/common/fiber.h"

class PipeStream : public Stream
{
    friend std::pair<Stream::ptr, Stream::ptr> pipeStream(size_t);
public:
    typedef boost::shared_ptr<PipeStream> ptr;
    typedef boost::weak_ptr<PipeStream> weak_ptr;

public:
    PipeStream(size_t bufferSize);
    ~PipeStream();

    bool supportsRead() { return true; }
    bool supportsWrite() { return true; }

    void close(CloseType type = BOTH);
    size_t read(Buffer &b, size_t len);
    size_t write(const Buffer &b, size_t len);
    void flush();

private:
    PipeStream::weak_ptr m_otherStream;
    boost::shared_ptr<boost::mutex> m_mutex;
    Buffer m_readBuffer;
    size_t m_bufferSize;
    CloseType m_closed, m_otherClosed;
    Scheduler *m_pendingWriterScheduler, *m_pendingReaderScheduler;
    Fiber::ptr m_pendingWriter, m_pendingReader;
};

std::pair<Stream::ptr, Stream::ptr> pipeStream(size_t bufferSize)
{
    if (bufferSize == ~0u)
        bufferSize = 65536;
    std::pair<PipeStream::ptr, PipeStream::ptr> result;
    result.first.reset(new PipeStream(bufferSize));
    result.second.reset(new PipeStream(bufferSize));
    result.first->m_otherStream = result.second;
    result.second->m_otherStream = result.first;
    result.first->m_mutex.reset(new boost::mutex());
    result.second->m_mutex = result.first->m_mutex;
    return result;
}

PipeStream::PipeStream(size_t bufferSize)
: m_bufferSize(bufferSize),
  m_closed(NONE),
  m_otherClosed(NONE)
{}

PipeStream::~PipeStream()
{
    boost::mutex::scoped_lock lock(*m_mutex);
    if (!m_otherStream.expired()) {
        PipeStream::ptr otherStream(m_otherStream);
        ASSERT(!otherStream->m_pendingReader);
        ASSERT(!otherStream->m_pendingWriter);
    }
    if (m_pendingReader) {
        m_pendingReaderScheduler->schedule(m_pendingReader);
        m_pendingReader.reset();
    }
    if (m_pendingWriter) {
        m_pendingWriterScheduler->schedule(m_pendingWriter);
        m_pendingWriter.reset();
    }
}

void
PipeStream::close(CloseType type)
{
    boost::mutex::scoped_lock lock(*m_mutex);
    m_closed = (CloseType)(m_closed | type);
    if (!m_otherStream.expired()) {
        PipeStream::ptr otherStream(m_otherStream);
        otherStream->m_otherClosed = m_closed;
    }
    if (m_pendingReader && (m_closed & WRITE)) {
        m_pendingReaderScheduler->schedule(m_pendingReader);
        m_pendingReader.reset();        
    }
    if (m_pendingWriter && (m_closed & READ)) {
        m_pendingWriterScheduler->schedule(m_pendingWriter);
        m_pendingWriter.reset();
    }
}

size_t
PipeStream::read(Buffer &b, size_t len)
{
    {
        boost::mutex::scoped_lock lock(*m_mutex);
        if (m_closed & READ)
            throw BadHandleException();
        if (m_otherStream.expired() && !(m_otherClosed & WRITE)) {
            throw BrokenPipeException();
        }
        size_t avail = m_readBuffer.readAvailable();
        if (avail > 0) {
            size_t todo = std::min(len, avail);
            b.copyIn(m_readBuffer, todo);
            m_readBuffer.consume(todo);
            if (m_pendingWriter) {
                m_pendingWriterScheduler->schedule(m_pendingWriter);
                m_pendingWriter.reset();
            }
            return todo;
        }

        if (m_otherClosed & WRITE) {
            return 0;
        }
        PipeStream::ptr otherStream(m_otherStream);

        // Wait for the other stream to schedule us
        ASSERT(!otherStream->m_pendingReader);
        otherStream->m_pendingReader = Fiber::getThis();
        otherStream->m_pendingReaderScheduler = Scheduler::getThis();
    }
    Scheduler::getThis()->yieldTo();
    // And recurse
    return read(b, len);
}

size_t
PipeStream::write(const Buffer &b, size_t len)
{
    if (len > m_bufferSize)
        len = m_bufferSize;
    while (true) {
        {
            boost::mutex::scoped_lock lock(*m_mutex);
            if (m_closed & WRITE)
                throw BadHandleException();
            if (m_otherStream.expired()) {
                throw BrokenPipeException();
            }
            PipeStream::ptr otherStream(m_otherStream);
            if (otherStream->m_closed & READ) {
                throw BrokenPipeException();            
            }

            if (otherStream->m_readBuffer.readAvailable() + len <= m_bufferSize) {
                otherStream->m_readBuffer.copyIn(b, len);
                if (m_pendingReader) {
                    m_pendingReaderScheduler->schedule(m_pendingReader);
                    m_pendingReader.reset();
                }            
                return len;
            }
            // Wait for the other stream to schedule us
            ASSERT(!otherStream->m_pendingWriter);
            otherStream->m_pendingWriter = Fiber::getThis();
            otherStream->m_pendingWriterScheduler = Scheduler::getThis();
        }
        Scheduler::getThis()->yieldTo();
    }
}

void
PipeStream::flush()
{
    while (true) {
        {
            boost::mutex::scoped_lock lock(*m_mutex);
            if (m_otherStream.expired()) {
                throw BrokenPipeException();
            }
            PipeStream::ptr otherStream(m_otherStream);
            if (otherStream->m_closed & READ) {
                throw BrokenPipeException();            
            }

            if (otherStream->m_readBuffer.readAvailable() == 0) {
                return;
            }
            // Wait for the other stream to schedule us
            ASSERT(!otherStream->m_pendingWriter);
            otherStream->m_pendingWriter = Fiber::getThis();
            otherStream->m_pendingWriterScheduler = Scheduler::getThis();
        }
        Scheduler::getThis()->yieldTo();
    }
}
