/*
 * mcpstdio.h - Qucs-S's tools as an MCP server on stdin and stdout
 *              (qucs-s --mcp-server): no window on screen, for a client
 *              that starts it - `claude mcp add qucs -- qucs-s --mcp-server`
 *              - batch jobs, checks of schematics, agents working on
 *              copies side by side.
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_MCPSTDIO_H
#define QUCS_MCPSTDIO_H

class QApplication;
class QTimer;
class QucsApp;

namespace qucs_s::mcp {

/// Whether the command line asks for the server (--mcp-server).
bool askedFor(int argc, char* argv[]);
/// Before the application is made: no window on screen (the offscreen
/// platform), and this instance's dock runs no claude and no gh.
void prepareHeadless();
/// A timer that closes what would ask at the start (a message box of the
/// main window's), until the client's first message.
QTimer* closeDialogsWhileStarting();
/// Serves the tools of \a window's QucsControl on stdin and stdout (JSON-
/// RPC, one message a line) until stdin ends; the application's exit code.
int runStdio(QApplication& app, QucsApp* window, QTimer* startupCloser);

} // namespace qucs_s::mcp

#endif // QUCS_MCPSTDIO_H
