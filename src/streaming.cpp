/***************************************************************************
 * ekho-linux -- streaming session. See streaming.h.
 * SPDX-License-Identifier: GPL-3.0-or-later
 **************************************************************************/
#include "streaming.h"

#include <errno.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include <algorithm>
#include <cstdio>
#include <vector>

#include "text_normalize.h"

namespace ekho {

long long nowUs() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return static_cast<long long>(tv.tv_sec) * 1000000LL + tv.tv_usec;
}

// ---------------------------------------------------------------------------
// PcmRing
// ---------------------------------------------------------------------------

PcmRing::PcmRing(size_t capacitySamples)
    : aborted_(false),
      buf_(new short[capacitySamples]),
      capacity_(capacitySamples),
      readPos_(0),
      writePos_(0),
      count_(0),
      dropped_(0) {
  pthread_mutex_init(&mutex_, nullptr);
  pthread_cond_init(&space_, nullptr);
}

PcmRing::~PcmRing() {
  pthread_cond_destroy(&space_);
  pthread_mutex_destroy(&mutex_);
  delete[] buf_;
}

size_t PcmRing::write(const short* pcm, size_t frames) {
  if (frames == 0) return 0;
  pthread_mutex_lock(&mutex_);

  size_t space = capacity_ - count_;
  if (frames > space) {
    // Drop-oldest: what survives is the newest min(frames, capacity_) frames
    // of the burst. Audio the consumer will never hear -- ring content
    // evicted here plus the head of a burst larger than the ring itself --
    // is counted in dropped_. The old code clamped the eviction to what was
    // stored and then copied from the head of the burst, which kept the
    // OLDEST frames of an oversized burst and reported dropped = 0.
    if (frames <= capacity_) {
      size_t evict = frames - space;
      readPos_ = (readPos_ + evict) % capacity_;
      count_ -= evict;
      dropped_ += evict;
    } else {
      // The burst alone overfills the ring: it replaces everything.
      dropped_ += count_ + (frames - capacity_);
      count_ = 0;
      readPos_ = writePos_;
    }
    space = capacity_ - count_;
  }
  size_t n = std::min(frames, space);
  if (frames > capacity_) {
    // Copy only the tail of the burst; its head never enters the ring.
    pcm += frames - capacity_;
    n = capacity_;
  }
  for (size_t i = 0; i < n; i++) {
    buf_[writePos_] = pcm[i];
    writePos_ = (writePos_ + 1) % capacity_;
  }
  count_ += n;
  pthread_mutex_unlock(&mutex_);
  return n;
}

size_t PcmRing::writeBlocking(const short* pcm, size_t frames) {
  // A zero-capacity ring has no space to wait for: without this guard the
  // loop parks forever (and write() would divide by the capacity).
  if (frames == 0 || capacity_ == 0) return 0;
  pthread_mutex_lock(&mutex_);

  size_t written = 0;
  while (written < frames) {
    while (count_ == capacity_ && !aborted_) {
      // Park until the consumer frees room. read() and clear() broadcast.
      pthread_cond_wait(&space_, &mutex_);
    }
    if (aborted_) break;

    size_t n = std::min(frames - written, capacity_ - count_);
    for (size_t i = 0; i < n; i++) {
      buf_[writePos_] = pcm[written + i];
      writePos_ = (writePos_ + 1) % capacity_;
    }
    count_ += n;
    written += n;
  }

  pthread_mutex_unlock(&mutex_);
  return written;
}

size_t PcmRing::read(short* out, size_t frames) {
  if (frames == 0) return 0;
  pthread_mutex_lock(&mutex_);
  size_t n = std::min(frames, count_);
  for (size_t i = 0; i < n; i++) {
    out[i] = buf_[readPos_];
    readPos_ = (readPos_ + 1) % capacity_;
  }
  count_ -= n;
  // Any space we just freed is room a blocked producer is waiting for.
  if (n > 0) pthread_cond_broadcast(&space_);
  pthread_mutex_unlock(&mutex_);
  return n;
}

void PcmRing::clear() {
  pthread_mutex_lock(&mutex_);
  readPos_ = writePos_ = count_ = 0;
  pthread_cond_broadcast(&space_);
  pthread_mutex_unlock(&mutex_);
}

void PcmRing::abort() {
  pthread_mutex_lock(&mutex_);
  aborted_ = true;
  pthread_cond_broadcast(&space_);
  pthread_mutex_unlock(&mutex_);
}

size_t PcmRing::available() const {
  pthread_mutex_lock(&mutex_);
  size_t n = count_;
  pthread_mutex_unlock(&mutex_);
  return n;
}

size_t PcmRing::droppedFrames() const {
  pthread_mutex_lock(&mutex_);
  size_t n = dropped_;
  pthread_mutex_unlock(&mutex_);
  return n;
}

// ---------------------------------------------------------------------------
// StreamingSession
// ---------------------------------------------------------------------------

StreamingSession::StreamingSession(Ekho* engine, size_t ringSamples)
    : engine_(engine),
      ring_(ringSamples),
      segmented_(true),
      normalize_(true),
      sealed_(false),
      ended_(false),
      stopped_(false),
      minTailChars_(6),
      maxSegmentChars_(48),
      firstAppendUs_(0) {
  pthread_mutex_init(&stateMutex_, nullptr);
  pthread_cond_init(&dataCond_, nullptr);
}

StreamingSession::~StreamingSession() {
  pthread_cond_destroy(&dataCond_);
  pthread_mutex_destroy(&stateMutex_);
}

int StreamingSession::synthTrampoline(short* pcm, int frames, void* arg,
                                      OverlapType type) {
  StreamingSession* self = static_cast<StreamingSession*>(arg);
  if (frames <= 0) return frames;

  // Record the first-chunk latency here rather than after synth4() returns.
  // The benchmark's first_ms is "when was there playable audio", and for a
  // long segment that is well before the segment finishes.
  pthread_mutex_lock(&self->stateMutex_);
  if (self->stats_.firstChunkUs < 0 && self->firstAppendUs_ != 0) {
    self->stats_.firstChunkUs = nowUs() - self->firstAppendUs_;
  }
  pthread_mutex_unlock(&self->stateMutex_);

  // Blocking write: applies back pressure instead of dropping samples.
  size_t wrote = self->ring_.writeBlocking(pcm, static_cast<size_t>(frames));
  pthread_cond_broadcast(&self->dataCond_);
  return static_cast<int>(wrote) == frames ? frames : 0;
}

int StreamingSession::synthSegment(const std::string& segment) {
  if (segment.empty()) return 0;

  long long t0 = nowUs();
  engine_->synth4(segment, &StreamingSession::synthTrampoline, this);
  long long t1 = nowUs();

  pthread_mutex_lock(&stateMutex_);
  stats_.genUs += (t1 - t0);
  stats_.segments++;
  stats_.lastChunkUs = t1 - firstAppendUs_;
  if (stats_.firstChunkUs < 0 && ring_.available() > 0) {
    stats_.firstChunkUs = t1 - firstAppendUs_;
  }
  pthread_mutex_unlock(&stateMutex_);

  pthread_cond_broadcast(&dataCond_);
  return 1;
}

size_t StreamingSession::deriveMaxSegmentChars(size_t ringFrames,
                                             int sampleRate,
                                             double secondsPerChar,
                                             double safety) {
  if (ringFrames == 0 || sampleRate <= 0 || secondsPerChar <= 0.0) return 0;
  double ringSeconds = static_cast<double>(ringFrames) / sampleRate;
  double chars = ringSeconds / secondsPerChar * safety;
  size_t n = static_cast<size_t>(chars);
  if (n < 4) n = 4;  // below this the per-segment overhead dominates
  return n;
}

// Number of trailing bytes in `s` that form an incomplete UTF-8 sequence.
static size_t trailingPartialUtf8(const std::string& s) {
  // Walk back at most 4 bytes looking for a lead byte whose sequence length
  // runs past the end of the string.
  for (size_t back = 0; back < 4 && back < s.size(); back++) {
    unsigned char c = static_cast<unsigned char>(s[s.size() - 1 - back]);
    if ((c & 0xC0) == 0x80) continue;  // continuation byte, keep walking back
    size_t need = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
    // back counts the bytes after the lead byte, so the sequence length
    // available including the lead byte is back + 1.
    size_t have = back + 1;
    if (have >= need) return 0;  // complete
    return have;                 // incomplete: hold these back
  }
  return 0;
}

int StreamingSession::append(const std::string& text, bool normalize) {
  long long t0 = nowUs();

  pthread_mutex_lock(&stateMutex_);
  if (stopped_ || sealed_ || ended_) {
    pthread_mutex_unlock(&stateMutex_);
    return 0;
  }
  if (firstAppendUs_ == 0) firstAppendUs_ = t0;
  stats_.lastChunkUs = t0 - firstAppendUs_;
  pthread_mutex_unlock(&stateMutex_);

  // Re-attach bytes held back from the previous call BEFORE the new text. Those
  // bytes are the head of a character whose tail is at the start of `text`, so
  // they have to be adjacent to it to form a whole character again. An earlier
  // revision erased them from buffer_ and then appended `text` after, which
  // dropped them: a byte-chunked caller lost one character every time a
  // multi-byte sequence straddled a boundary.
  std::string incoming = (normalize_ && normalize)
                             ? normalizeText(text, NormalizeOptions())
                             : text;
  buffer_ += pendingBytes_;
  pendingBytes_.clear();
  buffer_ += incoming;

  // Hold back a trailing partial UTF-8 sequence so a byte-chunked caller
  // cannot hand the front end half a character.
  size_t partial = trailingPartialUtf8(buffer_);
  if (partial > 0) {
    pendingBytes_ = buffer_.substr(buffer_.size() - partial);
    buffer_.erase(buffer_.size() - partial);
  }

  int made = 0;

  if (!segmented_) {
    if (!buffer_.empty()) {
      made += synthSegment(buffer_);
      buffer_.clear();
    }
  } else {
    // Byte budget for the cap. CJK is 3 bytes/char, so this is an upper bound
    // on characters; ASCII-heavy text will simply take longer to hit.
    const size_t kCapBytes = maxSegmentChars_ * 3;

    for (;;) {
      bool overCap = (maxSegmentChars_ > 0) && (buffer_.size() > kCapBytes);
      // The scan window is the cap itself when we are over it, so both the
      // boundary search and the forced cut land at the cap rather than at the
      // end of the whole buffer.
      size_t window = overCap ? kCapBytes : buffer_.size();
      if (window > buffer_.size()) window = buffer_.size();
      CutPoint cut = lastSafeCut(buffer_, 0, window, overCap);
      if (cut.offset == 0) break;
      // Do not emit a segment so small it is just one or two syllables.
      if (charCount(buffer_, 0, cut.offset) < 2) break;

      std::string segment = buffer_.substr(0, cut.offset);
      buffer_.erase(0, cut.offset);
      made += synthSegment(segment);
    }
  }

  long long t1 = nowUs();
  pthread_mutex_lock(&stateMutex_);
  stats_.appendUs += (t1 - t0);
  pthread_mutex_unlock(&stateMutex_);
  pthread_cond_broadcast(&dataCond_);

  return made;
}

int StreamingSession::flush() {
  pthread_mutex_lock(&stateMutex_);
  if (ended_ || sealed_ || stopped_) {
    pthread_mutex_unlock(&stateMutex_);
    return 0;
  }
  // Stop accepting new text now, but do not publish ended_ until the tail has
  // actually been synthesised: read() and finished() read "ended_ with an empty
  // ring" as end-of-stream, so a consumer already parked in read() would
  // otherwise return 0 the moment it woke and the final segment would be
  // produced into a ring with no reader.
  sealed_ = true;
  pthread_mutex_unlock(&stateMutex_);

  int made = 0;
  // flush() means no more text is coming, so a held-back partial character is
  // the final state of the stream and must be synthesised as-is.
  if (!pendingBytes_.empty()) {
    buffer_ += pendingBytes_;
    pendingBytes_.clear();
  }
  if (!buffer_.empty()) {
    made += synthSegment(buffer_);
    buffer_.clear();
  }

  // Release the tail the resampler is still holding, so it belongs to *this*
  // stream rather than appearing at the front of the next one. Without this,
  // two identical requests produce two different waveforms: see the note on
  // Ekho::drainAudio().
  if (!stopped_) {
    engine_->drainAudio(&StreamingSession::synthTrampoline, this);
  }

  pthread_mutex_lock(&stateMutex_);
  ended_ = true;
  pthread_mutex_unlock(&stateMutex_);

  pthread_cond_broadcast(&dataCond_);
  return made;
}

int StreamingSession::read(short* out, int frames, int timeoutMs) {
  if (frames <= 0) return 0;
  size_t got = ring_.read(out, static_cast<size_t>(frames));
  if (got > 0) return static_cast<int>(got);
  if (timeoutMs == 0) return 0;

  pthread_mutex_lock(&stateMutex_);
  while (!stopped_ && ring_.available() == 0) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeoutMs / 1000;
    ts.tv_nsec += static_cast<long>(timeoutMs % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
      ts.tv_sec += 1;
      ts.tv_nsec -= 1000000000L;
    }
    if (pthread_cond_timedwait(&dataCond_, &stateMutex_, &ts) == ETIMEDOUT) break;
  }
  bool done = stopped_ || (ended_ && ring_.available() == 0);
  pthread_mutex_unlock(&stateMutex_);

  if (done && ring_.available() == 0) return 0;
  return static_cast<int>(ring_.read(out, static_cast<size_t>(frames)));
}

void StreamingSession::stop() {
  pthread_mutex_lock(&stateMutex_);
  stopped_ = true;
  ended_ = true;
  buffer_.clear();
  pthread_mutex_unlock(&stateMutex_);
  // Release any producer parked on a full ring; otherwise it would wait for a
  // consumer that has been told to go away.
  ring_.abort();
  pthread_cond_broadcast(&dataCond_);
}

bool StreamingSession::finished() const {
  pthread_mutex_lock(&stateMutex_);
  bool f = (ended_ || stopped_) && ring_.available() == 0;
  pthread_mutex_unlock(&stateMutex_);
  return f;
}

StreamStats StreamingSession::stats() const {
  pthread_mutex_lock(&stateMutex_);
  StreamStats s = stats_;
  pthread_mutex_unlock(&stateMutex_);
  s.droppedFrames = ring_.droppedFrames();
  return s;
}

void StreamingSession::resetStats() {
  pthread_mutex_lock(&stateMutex_);
  stats_ = StreamStats();
  firstAppendUs_ = 0;
  pthread_mutex_unlock(&stateMutex_);
}

size_t StreamingSession::backlogChars() const {
  pthread_mutex_lock(&stateMutex_);
  size_t n = buffer_.size();
  pthread_mutex_unlock(&stateMutex_);
  return n;
}

}  // namespace ekho