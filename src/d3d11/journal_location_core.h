#pragma once
// The journal's Location line, read as a byte stream (journal_watch.cpp feeds it; the rig tools\location_scan_test compiles this
// header alone). The live journal writes a Location line as
//     "event":"Location", "DistFromStarLS":<number>, "Docked":true|false, ...
// so the Docked key sits about fifty bytes after the event token, and a read can end anywhere in between. The old scanner searched
// one read's bytes for the key, and a 32-byte carry could not cover the gap: a read that ended inside it lost the value, and the
// value reads false. This scanner keeps its state from one byte to the next, so the answer does not depend on where the reads
// split the file. feedChunk drops the bytes that an earlier read already gave it (a replayed file, or an overlap), so every byte
// is fed exactly once, in order.
#include <cstdint>

namespace edvr::journal_location {

class LocationScan {
public:
    // Feeds the bytes at absolute positions base..base+n-1. The bytes below next() were fed by an earlier read and are skipped.
    // Returns the number of bytes fed.
    uint32_t feedChunk(const uint8_t* p, uint32_t n, uint64_t base) noexcept {
        uint32_t fed = 0;
        for (uint32_t i = 0; i < n; ++i) {
            const uint64_t at = base + i;
            if (at < next_) continue;
            next_ = at + 1;
            feed(p[i]);
            ++fed;
        }
        return fed;
    }

    // Starts over: the next byte fed is position 0 (a file read again from the top).
    void reset() noexcept { *this = LocationScan(); }

    // Whether a Location line's Docked value has been read, and its value.
    bool known() const noexcept { return known_; }
    bool docked() const noexcept { return docked_; }
    // The absolute position of the next byte this scanner will take.
    uint64_t next() const noexcept { return next_; }

private:
    static constexpr char kEvent[] = "\"event\":\"Location\"";  // 18 bytes; its last byte is '"'
    static constexpr uint32_t kEventLen = sizeof(kEvent) - 1;
    static constexpr char kDocked[] = "\"Docked\":";             // 9 bytes; its last byte is ':'
    static constexpr uint32_t kDockedLen = sizeof(kDocked) - 1;
    static constexpr uint32_t kTail = 32;                       // the last bytes kept for the matches
    enum : uint8_t { kNone = 0, kValueGap = 1, kWord = 2 };     // stages: looking for the key, skipping spaces, matching a word

    void push(uint8_t c) noexcept {
        tail_[tailAt_] = c;
        tailAt_ = (tailAt_ + 1) % kTail;
        if (tailLen_ < kTail) ++tailLen_;
    }

    // Whether the last n bytes equal pat (the caller checks the last byte first, so this runs rarely).
    bool endsWith(const char* pat, uint32_t n) const noexcept {
        if (tailLen_ < n) return false;
        for (uint32_t k = 0; k < n; ++k) {
            const uint32_t idx = (tailAt_ + kTail - n + k) % kTail;
            if (tail_[idx] != static_cast<uint8_t>(pat[k])) return false;
        }
        return true;
    }

    void feed(uint8_t c) noexcept {
        push(c);
        if (c == '\n') {            // a line ends: the event and the key do not carry over to the next line
            inLocation_ = false;
            stage_ = kNone;
            return;
        }
        if (stage_ == kWord) {      // matching "true" or "false" after the key
            if (c == static_cast<uint8_t>(word_[wordAt_])) {
                if (++wordAt_ == wordLen_) {
                    known_ = true;
                    docked_ = wordLen_ == 4;
                    inLocation_ = false;
                    stage_ = kNone;
                }
            } else {
                stage_ = kNone;     // not a boolean: the Docked value is not read from this line
            }
            return;
        }
        if (stage_ == kValueGap) {  // the key is matched; spaces may precede the value
            if (c == ' ') return;
            if (c == 't') {
                word_ = "true";
                wordLen_ = 4;
                wordAt_ = 1;
                stage_ = kWord;
                return;
            }
            if (c == 'f') {
                word_ = "false";
                wordLen_ = 5;
                wordAt_ = 1;
                stage_ = kWord;
                return;
            }
            stage_ = kNone;
            return;
        }
        // kNone: the event token marks a Location line; its Docked key may follow later on the same line.
        if (c == '"' && endsWith(kEvent, kEventLen)) {
            inLocation_ = true;
            return;
        }
        if (inLocation_ && c == ':' && endsWith(kDocked, kDockedLen)) stage_ = kValueGap;
    }

    uint8_t tail_[kTail] = {};
    uint32_t tailAt_ = 0;
    uint32_t tailLen_ = 0;
    uint64_t next_ = 0;
    bool inLocation_ = false;
    bool known_ = false;
    bool docked_ = false;
    uint8_t stage_ = kNone;
    const char* word_ = "";
    uint32_t wordLen_ = 0;
    uint32_t wordAt_ = 0;
};

}  // namespace edvr::journal_location
