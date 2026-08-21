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

// httplib.h is included FIRST (before any other header) so that on Windows its
// <winsock2.h> is pulled in ahead of any <windows.h>. This TU deliberately
// includes NO hpl headers - it must not call hpl::Log() (not thread-safe) off
// the detached thread; the LevelEditor logs receipt on its own main thread.
#include "httplib.h"

#ifndef RAPIDJSON_HAS_STDSTRING
#define RAPIDJSON_HAS_STDSTRING 1
#endif
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"

#include "ModelEditorReloadNotify.h"

#include <thread>

//----------------------------------------------------------------------------

void ModelEditorNotifyReload(const std::string& asHost, int alPort,
                             const std::string& asToken, const std::string& asEntFile)
{
	if(alPort<=0 || asEntFile.empty())
		return;

	const std::string sHost = asHost.empty() ? std::string("127.0.0.1") : asHost;

	//////////////////////////////////////////////////////////////////////
	// Build the JSON-RPC 2.0 tools/call envelope for reload_entity_file.
	rapidjson::StringBuffer buf;
	rapidjson::Writer<rapidjson::StringBuffer> w(buf);
	w.StartObject();
	w.Key("jsonrpc"); w.String("2.0");
	w.Key("id");      w.Int(1);
	w.Key("method");  w.String("tools/call");
	w.Key("params");
	w.StartObject();
	w.Key("name");      w.String("reload_entity_file");
	w.Key("arguments");
	w.StartObject();
	w.Key("path");    w.String(asEntFile.c_str(), (rapidjson::SizeType)asEntFile.size());
	w.EndObject();
	w.EndObject();
	w.EndObject();
	const std::string sBody(buf.GetString(), buf.GetSize());
	const std::string sToken = asToken;

	//////////////////////////////////////////////////////////////////////
	// Detached so a save never blocks on the socket. Captures only value
	// copies and touches no editor/engine state, so it's safe off-thread.
	std::thread([sHost, alPort, sToken, sBody]()
	{
		httplib::Client cli(sHost, alPort);
		cli.set_connection_timeout(1, 0);
		cli.set_write_timeout(2, 0);
		cli.set_read_timeout(3, 0);

		httplib::Headers headers;
		if(sToken.empty()==false)
			headers.emplace("Authorization", std::string("Bearer ") + sToken);

		// Result intentionally ignored (see file header re: logging).
		cli.Post("/mcp", headers, sBody, "application/json");
	}).detach();
}
