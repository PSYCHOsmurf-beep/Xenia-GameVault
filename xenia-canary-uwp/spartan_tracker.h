// RPCS3 Spartan Test trackers, ported to Xenia (PSYCHOsmurf / RPCS3 Spartan Test).
// Released under the BSD license, like Xenia - see LICENSE in the root.
#ifndef XENIA_UWP_SPARTAN_TRACKER_H_
#define XENIA_UWP_SPARTAN_TRACKER_H_

namespace xe {
class Emulator;
}

namespace spartan {

// Writes the trackers' start line, installs the crash handler and starts the memory and audio watch (one thread,
// for the life of the app). Call once, after the emulator is set up.
void StartTrackers(xe::Emulator* emulator);

}  // namespace spartan

#endif  // XENIA_UWP_SPARTAN_TRACKER_H_
