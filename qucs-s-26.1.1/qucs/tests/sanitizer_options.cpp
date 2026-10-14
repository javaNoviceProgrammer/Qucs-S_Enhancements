/*
 * AddressSanitizer's options for a test, on macOS: allocation stacks of
 * eight frames, not thirty.
 *
 * A process started there is forked, and macOS's own fork handlers
 * (notify, xpc, dispatch, CoreServices) allocate and free in the child,
 * before it runs the program - through the sanitizer's allocator, which
 * stores each stack in its depot (on Linux nothing allocates there). The
 * sanitizer does not hold the depot's lock across fork() on macOS: a fork
 * while another thread is storing a stack - the thread pool's git, a file
 * model's gatherer - and the child dies in fork(), its lock's owner gone
 * ("BUG IN CLIENT OF LIBPLATFORM: os_unfair_lock is corrupt", "crashed on
 * child side of fork pre-exec"). QProcess sees the program killed: a git
 * in some thousands, and test_git_integration, which runs some 1700,
 * failed one check or another in about one run in three.
 *
 * Eight frames reach from the child's allocation to QProcess, never past
 * it (nine to QProcessPrivate::start the fewest): every fork's are the
 * same stacks, in the depot since the first, found without its lock. A
 * report keeps its stacks' top eight frames; ASAN_OPTIONS sets it as ever
 * (malloc_context_size=30 for whole ones).
 */
#if defined(__APPLE__)
#if defined(__SANITIZE_ADDRESS__)
#define QUCS_TEST_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define QUCS_TEST_ASAN 1
#endif
#endif
#endif

#ifdef QUCS_TEST_ASAN
extern "C" __attribute__((visibility("default"), used)) const char* __asan_default_options()
{
    return "malloc_context_size=8";
}
#endif
