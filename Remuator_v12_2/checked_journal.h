#pragma once
#include <cstring>
#include <memory>
#include <stddef.h>
#include <stdint.h>

// Storage supplies exact-size reads, complete writes and atomic replacement.
// Each slot contains both the execution phase and its immutable program snapshot.
template <class Storage, class Payload> class CheckedJournal {
  struct Header {
    uint32_t magic;
    uint32_t version;
    uint32_t size;
    uint32_t checksum;
    uint64_t sequence;
  };
  struct Packet {
    Header header;
    Payload payload;
  };
  Storage &storage;
  uint64_t sequence = 0;
  int activeSlot = -1;
  const char *slot(int index) const {
    return index == 0 ? "/state/execution-a.bin" : "/state/execution-b.bin";
  }
  static uint32_t hash(const void *data, size_t length) {
    const uint8_t *bytes = static_cast<const uint8_t *>(data);
    uint32_t result = 2166136261UL;
    for (size_t i = 0; i < length; ++i) {
      result ^= bytes[i];
      result *= 16777619UL;
    }
    return result;
  }
  bool read(const char *path, Packet &packet) {
    if (!storage.read(path, &packet, sizeof(packet)))
      return false;
    if (packet.header.magic != 0x524D4A31UL || packet.header.version != 1 ||
        packet.header.size != sizeof(Payload) || !packet.header.sequence)
      return false;
    const uint32_t expected = packet.header.checksum;
    packet.header.checksum = 0;
    const bool ok = hash(&packet, sizeof(packet)) == expected;
    packet.header.checksum = expected;
    return ok;
  }

public:
  explicit CheckedJournal(Storage &source) : storage(source) {}
  uint64_t lastSequence() const { return sequence; }
  int lastSlot() const { return activeSlot; }
  void continueAfter(uint64_t previousSequence, int previousSlot) {
    sequence = previousSequence;
    activeSlot = previousSlot;
  }
  bool hasRecords() {
    return storage.exists(slot(0)) || storage.exists(slot(1)) ||
           storage.exists("/state/execution.tmp");
  }
  bool load(Payload &result, bool &degraded) {
    std::unique_ptr<Packet> packet(new Packet{});
    sequence = 0;
    activeSlot = -1;
    degraded = storage.exists("/state/execution.tmp");
    for (int i = 0; i < 2; ++i) {
      if (!storage.exists(slot(i)))
        continue;
      if (!read(slot(i), *packet)) {
        degraded = true;
        continue;
      }
      if (packet->header.sequence > sequence) {
        sequence = packet->header.sequence;
        activeSlot = i;
        result = packet->payload;
      }
    }
    return activeSlot >= 0;
  }
  bool save(const Payload &value) {
    if (sequence == UINT64_MAX || !storage.ensureDirectory())
      return false;
    std::unique_ptr<Packet> packet(new Packet{});
    packet->header.magic = 0x524D4A31UL;
    packet->header.version = 1;
    packet->header.size = sizeof(Payload);
    packet->header.sequence = sequence + 1;
    packet->payload = value;
    packet->header.checksum = hash(packet.get(), sizeof(Packet));
    const uint32_t checksum = packet->header.checksum;
    const char *temporary = "/state/execution.tmp";
    if (!storage.write(temporary, packet.get(), sizeof(Packet)) || !read(temporary, *packet) ||
        packet->header.checksum != checksum)
      return false;
    const int next = activeSlot == 0 ? 1 : 0;
    // LittleFS rename is atomic; do not delete the previous slot first.
    if (!storage.replace(temporary, slot(next)))
      return false;
    sequence = packet->header.sequence;
    activeSlot = next;
    return true;
  }
};
