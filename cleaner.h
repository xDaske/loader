#pragma once

// Entry points of the Destruct pass. Everything else stays private to
// cleaner.cpp; the overlay only needs these five calls.

namespace cleaner {

// Stops Windows from writing a Prefetch entry for this process. Call as early
// as possible, before any other work.
void disable_prefetch_for_current_process();

// Removes the Zone.Identifier alternate data stream ("mark of the web") from
// the running binary, so the download origin does not survive an install.
void strip_self_zone_identifier();

// Handles --destruct / --destruct-keep and their modifiers. Returns true when
// a CLI mode was handled; the process exits inside, so the caller should
// return immediately.
bool handle_cli();

// Runs the pass on a worker; the process ends itself when the pass is done.
// A second call is a no-op.
void run_and_exit_async(bool self_delete, bool clock_rollback);

// True while the async pass is running.
bool is_destructing();

} // namespace cleaner
