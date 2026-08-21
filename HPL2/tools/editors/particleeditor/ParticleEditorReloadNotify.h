/*
 * Copyright © 2009-2020 Frictional Games
 *
 * This file is part of Amnesia: The Dark Descent.
 *
 * Amnesia: The Dark Descent is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.

 * Amnesia: The Dark Descent is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Amnesia: The Dark Descent.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef PARTICLEEDITOR_RELOAD_NOTIFY_H
#define PARTICLEEDITOR_RELOAD_NOTIFY_H

#include <string>

//----------------------------------------------------------------------
// Fire-and-forget: POST a JSON-RPC tools/call for reload_particle_system(path) to
// the LevelEditor's MCP server at host:port, so it re-reads the just-saved .ps
// and updates its placed instances live. The socket I/O runs on a detached
// thread, so a save never blocks on it. No-op if alPort<=0 or asPsFile empty.
//
// std::string params (not tString) on purpose: this TU includes httplib.h first
// and pulls in NO hpl headers, so httplib's winsock include ordering can't clash
// with <windows.h> on Windows.
//----------------------------------------------------------------------
void ParticleEditorNotifyReload(const std::string& asHost, int alPort,
                                const std::string& asToken, const std::string& asPsFile);

#endif // PARTICLEEDITOR_RELOAD_NOTIFY_H
