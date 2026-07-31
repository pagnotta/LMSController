#include <pebble.h>

int main(void) {
  Window *w = window_create();
  window_stack_push(w, true);

  // Alloy's default XS machine is small: 8 KB chunk heap, 512 slots (8 KB) and
  // 384 stack slots -- roughly 32 KB, and it cannot grow (see "creation" in
  // build/devices/pebble/manifest.json of the Moddable tree). Navigating the
  // LMS menu ran into "fxAbort memory full" inside that budget.
  //
  // emery reports 130764 bytes of free app heap, so asking for more is fine.
  // Sizes here are in BYTES; the manifest counts slots.
  //
  // Measured on a physical Pebble Time 2 with 48K chunk / 24K slot / 6K stack,
  // right after the UI came up:
  //
  //   chunk 10420 / 49152   slot 16608 / 24560   stack 2560 / 6144
  //
  // So the slot heap, where promises and closures live, was already two thirds
  // gone at startup while the chunk heap was barely touched -- hence the extra
  // room for slots rather than chunks.
  //
  // NOTE: the QEMU emulator IGNORES this record entirely. Chunk sizes of 1 KB
  // and of 900 KB both behave exactly like the default there, and an allocation
  // probe could hold only 1 KB against 40 KB on real hardware. Do not use the
  // emulator to judge memory behaviour -- see src/embeddedjs/diag.js.
  ModdableCreationRecord cr = {
    .recordSize = sizeof(cr),
    .stack = 6 * 1024,
    .slot = 32 * 1024,
    .chunk = 48 * 1024,
    // Required for the XS counters read by src/embeddedjs/diag.js: without it
    // Instrumentation.get() resolves the names but always answers 0. On real
    // hardware it also emits the "instruments:" lines seen in `pebble logs`.
    .flags = kModdableCreationFlagLogInstrumentation,
  };

#ifdef PBL_DEBUG
  // Built with `pebble build --debug`: enable the xsbug JavaScript debugger.
  cr.flags |= kModdableCreationFlagDebug;
#endif

  moddable_createMachine(&cr);

  window_destroy(w);
}
