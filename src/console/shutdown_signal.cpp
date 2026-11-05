#include "shutdown_signal.hpp"

// sigaction, SIGINT, SIGTERM, SIGHUP are signal-related definitions
#include <csignal>

// Anumous namespace: the variables and functions defined here are only visible in this .cpp file
namespace {


// Received shutdown signal flag: 0 = no signal, 1 = signal received
// sig_atomic_t is a type that guarantees atomic access to an integer variable, meaning that it can be safely read and written in signal handlers without the risk of data races or inconsistencies.
// volatile tells the compiler not to optimize access to this variable.
volatile std::sig_atomic_t g_shutdown = 0;

// Signal handler function: just record "shutdown requested", the actual cleanup work is done by the main loop
void OnSignal(int) { g_shutdown = 1; }

}  // namespace

// Setting signal process: Ctrl+C（SIGINT）、kill（SIGTERM）、turn off terminal（SIGHUP）
void InstallShutdownSignals() {
    // Signal Process Setting
    struct sigaction sa {};
    // Once received, call OnSignal
    sa.sa_handler = OnSignal;
    // SA_RESETHAND: aims to reset the signal handler to the default action after the signal is handled once.
    // This means that if the signal is received again, the default action will be taken (e.g., termination).
    sa.sa_flags = SA_RESETHAND;
    // Process will not block any other signals while handling this signal
    sigemptyset(&sa.sa_mask);
    // Ctrl+C
    sigaction(SIGINT, &sa, nullptr);
    // kill <pid>
    sigaction(SIGTERM, &sa, nullptr);
    // Close terminal
    sigaction(SIGHUP, &sa, nullptr);
}

// Inquiry if a shutdown signal has been received
bool ShutdownRequested() {
    // Return true if a shutdown signal has been received
    return g_shutdown != 0;
}
