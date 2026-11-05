#pragma once


// Setting SIGNINT (Ctrl+C), SIGTERM (kill), SIGHUP (terminal close) handling:
//  - First time: just record "shutdown requested", and interrupt the waiting input, so the main loop can cleanly stop all child processes
//  - Second time: restore the system default behavior, directly terminate the manager (child processes will be terminated together due to PR_SET_PDE
void InstallShutdownSignals();

// Return true if a shutdown signal has been received
bool ShutdownRequested();
