/*
 * AddressSanitizer's options for a test - and the application, which links
 * it too -, on macOS: no allocation stacks kept (malloc_context_size=0).
 *
 * A process started there is forked, and macOS's own fork handlers
 * (notify, xpc, dispatch, CoreServices) allocate and free in the child,
 * before it runs the program - through the sanitizer's allocator, which
 * stores each stack in its depot (on Linux nothing allocates there). The
 * sanitizer does not hold the depot's locks across fork() on macOS: a fork
 * while another thread is storing a stack - the thread pool's git, a file
 * model's gatherer - and the child dies in fork(), its lock's owner gone
 * ("BUG IN CLIENT OF LIBPLATFORM: os_unfair_lock is corrupt", "crashed on
 * child side of fork pre-exec", in StackDepot::Put). QProcess sees the
 * program killed by signal 9: a git, a python. test_git_integration, which
 * starts some 1700, failed a check in about one run in three;
 * test_claude_fuzz, test_claude_code and the Python tests now and then.
 *
 * Eight frames (14 October) made it rarer, not gone: they were to make
 * every fork's stacks the same, stored at the first fork and found after
 * it without the lock. But a child stores into its own copy of the depot,
 * gone at its exec; the parent never runs the fork handlers, so never holds
 * their stacks, and every child stores them again, taking the locks. With
 * none kept nothing is stored: under load, 9 runs of 9 passed and no child
 * died (docs/bug_hunts/2026-11-08-asan-fork.md), where 4 of 9 failed with
 * eight frames. A report still has its access's stack, not where the memory
 * was allocated or freed: ASAN_OPTIONS=malloc_context_size=30 for those
 * (it overrides this), on a run that need not start processes, or on Linux.
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
    return "malloc_context_size=0";
}
#endif
