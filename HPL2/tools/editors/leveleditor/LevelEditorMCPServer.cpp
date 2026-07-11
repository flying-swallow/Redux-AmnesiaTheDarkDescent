/*
 * See LevelEditorMCPServer.h for the design + threading contract.
 *
 * httplib.h is included FIRST here (and in no other TU) so that on Windows its
 * <winsock2.h> is pulled in before any <windows.h> that hpl headers may include.
 */

#include "httplib.h"

#ifndef RAPIDJSON_HAS_STDSTRING
#define RAPIDJSON_HAS_STDSTRING 1
#endif
#include "rapidjson/document.h"
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"

#include <chrono>
#include <vector>

#include "LevelEditorMCPServer.h"
#include "LevelEditorMCPCommands.h"

#include "system/LowLevelSystem.h" // for Log()

using namespace hpl;

//--------------------------------------------------------------------

static const char* kMCPServerName    = "hpl2-leveleditor";
static const char* kMCPServerVersion = "0.1.0";
static const char* kMCPDefaultProto  = "2025-06-18";

// How long a worker thread waits for the main thread to process a tool call.
static const int   kMCPCallTimeoutSeconds = 15;

//--------------------------------------------------------------------
// JSON output helpers.
//
// Server.cpp only ever builds a handful of fixed, small envelope shapes and
// serializes them immediately (never mutating a DOM), so it emits JSON with
// RapidJSON's SAX Writer directly -- no allocator, no Document: RawValue()
// splices already-serialized fragments (the tool schema / tool content) with no
// reparse, and Value::Accept() re-emits the request id with its original JSON
// type. Request reads navigate the parsed rapidjson::Document via the
// NULL-safe JFind/JStrOf helpers (raw Value getters assert on a type
// mismatch, so every access type-checks first).
//--------------------------------------------------------------------

using JWriter = rapidjson::Writer<rapidjson::StringBuffer>;

// Echo a request id with its original JSON type; NULL -> json null.
static void WriteId(JWriter& w, const rapidjson::Value* apId)
{
	if(apId) apId->Accept(w);
	else     w.Null();
}

// A single-text-block MCP content array: [{"type":"text","text":<text>}].
static std::string TextContentBlock(const std::string& asText)
{
	rapidjson::StringBuffer buf;
	JWriter w(buf);
	w.StartArray();
	w.StartObject();
	w.Key("type"); w.String("text");
	w.Key("text"); w.String(asText);
	w.EndObject();
	w.EndArray();
	return std::string(buf.GetString(), buf.GetSize());
}

// Best-effort inverse of TextContentBlock: pull the first text block's string
// out of a serialized content array so it can be logged in one readable line.
// Falls back to the raw JSON if it isn't the shape we expect.
static std::string FirstTextBlock(const std::string& asContentJson)
{
	rapidjson::Document doc;
	doc.Parse(asContentJson.c_str(), asContentJson.size());
	if(doc.HasParseError()==false && doc.IsArray() && doc.Size()>0)
	{
		const rapidjson::Value& first = doc[0];
		if(first.IsObject() && first.HasMember("text") && first["text"].IsString())
			return std::string(first["text"].GetString(), first["text"].GetStringLength());
	}
	return asContentJson;
}

// {"jsonrpc":"2.0","id":<id>,"result":<body>} -- afWriteBody writes the result value.
template<class WriteBody>
static std::string MakeResult(const rapidjson::Value* apId, WriteBody afWriteBody)
{
	rapidjson::StringBuffer buf;
	JWriter w(buf);
	w.StartObject();
	w.Key("jsonrpc"); w.String("2.0");
	w.Key("id");      WriteId(w, apId);
	w.Key("result");  afWriteBody(w);
	w.EndObject();
	return std::string(buf.GetString(), buf.GetSize());
}

// {"jsonrpc":"2.0","id":<id>,"error":{"code":..,"message":..}}
static std::string MakeError(const rapidjson::Value* apId, int alCode, const std::string& asMsg)
{
	rapidjson::StringBuffer buf;
	JWriter w(buf);
	w.StartObject();
	w.Key("jsonrpc"); w.String("2.0");
	w.Key("id");      WriteId(w, apId);
	w.Key("error");
	w.StartObject();
	w.Key("code");    w.Int(alCode);
	w.Key("message"); w.String(asMsg);
	w.EndObject();
	w.EndObject();
	return std::string(buf.GetString(), buf.GetSize());
}

//--------------------------------------------------------------------

cLevelEditorMCPServer::cLevelEditorMCPServer(cLevelEditor* apEditor, const std::string& asBindAddr,
											 int alPort, const std::string& asToken)
	: mpEditor(apEditor)
	, mpCommands(new cLevelEditorMCPCommands(apEditor))
	, mpServer(NULL)
	, msBindAddr(asBindAddr)
	, mlPort(alPort)
	, msToken(asToken)
	, mbRunning(false)
{
}

//--------------------------------------------------------------------

cLevelEditorMCPServer::~cLevelEditorMCPServer()
{
	Stop();
	delete mpCommands;
}

//--------------------------------------------------------------------

bool cLevelEditorMCPServer::Start()
{
	if(mbRunning) return true;

	mpServer = new httplib::Server();

	mpServer->Post("/mcp", [this](const httplib::Request& req, httplib::Response& res)
	{
		// Optional shared-secret gate (Authorization: Bearer <token> or X-MCP-Token).
		if(msToken.empty()==false)
		{
			const std::string sAuth  = req.get_header_value("Authorization");
			const std::string sToken = req.get_header_value("X-MCP-Token");
			if(sAuth != ("Bearer " + msToken) && sToken != msToken)
			{
				res.status = 401;
				res.set_content("{\"error\":\"unauthorized\"}", "application/json");
				return;
			}
		}

		std::string sResponse = HandleJsonRpc(req.body);
		if(sResponse.empty())
			res.status = 202; // notification / no response body
		else
			res.set_content(sResponse, "application/json");
	});

	// This tools-only server never pushes server-initiated messages, so it
	// declines the optional SSE stream (spec-valid).
	mpServer->Get("/mcp", [](const httplib::Request&, httplib::Response& res)
	{
		res.status = 405;
		res.set_header("Allow", "POST");
	});

	if(mpServer->bind_to_port(msBindAddr, mlPort)==false)
	{
		Log("LevelEditor MCP: FAILED to bind %s:%d (port in use?)\n", msBindAddr.c_str(), mlPort);
		delete mpServer;
		mpServer = NULL;
		return false;
	}

	mbRunning = true;
	mListenThread = std::thread([this]()
	{
		mpServer->listen_after_bind();
	});

	Log("LevelEditor MCP: listening on http://%s:%d/mcp\n", msBindAddr.c_str(), mlPort);
	return true;
}

//--------------------------------------------------------------------

void cLevelEditorMCPServer::Stop()
{
	if(mbRunning==false && mpServer==NULL) return;

	mbRunning = false;

	if(mpServer)
		mpServer->stop();

	// Wake any worker threads still blocked on a queued call so their handler
	// returns promptly (otherwise the server dtor would block joining them).
	{
		std::lock_guard<std::mutex> lock(mQueueMutex);
		for(cQueuedCall& call : mlstQueue)
		{
			cMCPToolResult r;
			r.mbIsError = true;
			r.msContentJson = "[{\"type\":\"text\",\"text\":\"editor shutting down\"}]";
			try { call.mResult.set_value(r); } catch(...) {}
		}
		mlstQueue.clear();
	}

	// Wake any DEFERRED (async) calls still awaiting their job so their worker
	// threads unblock too. mDeferred is main-thread-only (no lock); Stop() runs
	// on the main thread.
	for(std::map<int, std::promise<cMCPToolResult>>::iterator it = mDeferred.begin();
		it != mDeferred.end(); ++it)
	{
		cMCPToolResult r;
		r.mbIsError = true;
		r.msContentJson = "[{\"type\":\"text\",\"text\":\"editor shutting down\"}]";
		try { it->second.set_value(r); } catch(...) {}
	}
	mDeferred.clear();

	if(mListenThread.joinable())
		mListenThread.join();

	delete mpServer;
	mpServer = NULL;

	Log("LevelEditor MCP: server stopped\n");
}

//--------------------------------------------------------------------

void cLevelEditorMCPServer::DrainQueue()
{
	// Move the pending calls out under the lock, then run handlers unlocked so
	// worker threads can keep enqueueing while we process on the main thread.
	std::deque<cQueuedCall> lPending;
	{
		std::lock_guard<std::mutex> lock(mQueueMutex);
		if(mlstQueue.empty()) return;
		lPending.swap(mlstQueue);
	}

	for(cQueuedCall& call : lPending)
	{
		// Log the tool name up front so a handler that crashes mid-call still
		// leaves a breadcrumb in the editor log.
		Log("LevelEditor MCP: tool '%s'\n", call.msTool.c_str());

		cMCPToolResult result;
		try
		{
			result = mpCommands->HandleToolCall(call.msTool, call.msArgsJson);
		}
		catch(const std::exception& e)
		{
			result.mbIsError = true;
			result.msContentJson = TextContentBlock(std::string("exception: ") + e.what());
		}
		catch(...)
		{
			result.mbIsError = true;
			result.msContentJson = "[{\"type\":\"text\",\"text\":\"unknown exception in tool handler\"}]";
		}

		if(result.mbDeferred)
		{
			Log("LevelEditor MCP: tool '%s' deferred (job %d)\n", call.msTool.c_str(), result.mlDeferJobId);
			// The tool started an async job (e.g. a camera capture render). Park
			// the caller's promise keyed by job id; FulfillDeferred() completes it
			// when the job finishes a few frames later. The worker thread stays
			// blocked on its future (15s timeout) meanwhile.
			mDeferred[result.mlDeferJobId] = std::move(call.mResult);
		}
		else
		{
			if(result.mbIsError)
				Warning("LevelEditor MCP: tool '%s' failed: %s\n", call.msTool.c_str(),
					FirstTextBlock(result.msContentJson).c_str());
			try { call.mResult.set_value(result); } catch(...) {}
		}
	}
}

//--------------------------------------------------------------------

void cLevelEditorMCPServer::FulfillDeferred(int alJobId, const cMCPToolResult& aResult)
{
	std::map<int, std::promise<cMCPToolResult>>::iterator it = mDeferred.find(alJobId);
	if(it == mDeferred.end()) return; // unknown / already fulfilled (e.g. by Stop)
	try { it->second.set_value(aResult); } catch(...) {}
	mDeferred.erase(it);
	Log("LevelEditor MCP: deferred job %d done\n", alJobId);
}

//--------------------------------------------------------------------
// Connection-snippet catalog (for the Options "MCP" tab)
//--------------------------------------------------------------------

static const char* kMCPClientLabels[] =
{
	"Claude Code", "VS Code", "Codex", "Cursor", "Windsurf", "curl", "mcp-remote"
};

int cLevelEditorMCPServer::GetClientCount()
{
	return (int)(sizeof(kMCPClientLabels) / sizeof(kMCPClientLabels[0]));
}

std::string cLevelEditorMCPServer::GetClientLabel(int alIdx)
{
	if(alIdx < 0 || alIdx >= GetClientCount()) return "";
	return kMCPClientLabels[alIdx];
}

std::string cLevelEditorMCPServer::BuildClientSnippet(int alIdx, const std::string& asHost, int alPort, const std::string& asToken)
{
	const std::string url    = "http://" + asHost + ":" + std::to_string(alPort) + "/mcp";
	const bool        hasTok = asToken.empty()==false;

	switch(alIdx)
	{
		case 0: // Claude Code CLI
		{
			std::string s = "claude mcp add -t http leveleditor " + url;
			if(hasTok) s += " -H \"Authorization: Bearer " + asToken + "\"";
			return s;
		}
		case 1: // VS Code CLI (inner JSON quotes escaped for the shell)
		{
			std::string j = "{\\\"name\\\":\\\"leveleditor\\\",\\\"type\\\":\\\"http\\\",\\\"url\\\":\\\"" + url + "\\\"";
			if(hasTok) j += ",\\\"headers\\\":{\\\"Authorization\\\":\\\"Bearer " + asToken + "\\\"}";
			j += "}";
			return "code --add-mcp \"" + j + "\"";
		}
		case 2: // Codex CLI
		{
			std::string s = "codex mcp add leveleditor --url " + url;
			if(hasTok) s += " --bearer-token-env-var MCP_TOKEN";
			return s;
		}
		case 3: // Cursor  (~/.cursor/mcp.json, one-line)
		{
			std::string s = "{\"mcpServers\":{\"leveleditor\":{\"url\":\"" + url + "\"";
			if(hasTok) s += ",\"headers\":{\"Authorization\":\"Bearer " + asToken + "\"}";
			s += "}}}";
			return s;
		}
		case 4: // Windsurf  (~/.codeium/windsurf/mcp_config.json, one-line)
			return "{\"mcpServers\":{\"leveleditor\":{\"serverUrl\":\"" + url + "\"}}}";
		case 5: // raw curl (initialize)
		{
			std::string s = "curl -sS " + url + " -H \"Content-Type: application/json\"";
			if(hasTok) s += " -H \"Authorization: Bearer " + asToken + "\"";
			s += " -d '{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
				 "\"params\":{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},"
				 "\"clientInfo\":{\"name\":\"curl\",\"version\":\"1\"}}}'";
			return s;
		}
		case 6: // mcp-remote stdio bridge (for stdio-only clients)
		{
			std::string s = "npx mcp-remote " + url;
			if(hasTok) s += " --header \"Authorization: Bearer " + asToken + "\"";
			return s;
		}
	}
	return "";
}

//--------------------------------------------------------------------

// NULL-safe member lookup.
static const rapidjson::Value* JFind(const rapidjson::Value& aObj, const char* asKey)
{
	if(aObj.IsObject()==false) return NULL;
	rapidjson::Value::ConstMemberIterator it = aObj.FindMember(asKey);
	return it!=aObj.MemberEnd() ? &it->value : NULL;
}
static std::string JStrOf(const rapidjson::Value* v, const std::string& asDef="")
{
	return (v && v->IsString()) ? std::string(v->GetString(), v->GetStringLength()) : asDef;
}

std::string cLevelEditorMCPServer::HandleJsonRpc(const std::string& asBody)
{
	rapidjson::Document reqDoc;
	reqDoc.Parse(asBody.c_str(), asBody.size());
	if(reqDoc.HasParseError())
		return MakeError(NULL, -32700, "Parse error");

	// Process a single JSON-RPC request object. Returns an empty string for
	// notifications (no response -> the caller replies HTTP 202).
	auto processOne = [this](const rapidjson::Value& r) -> std::string
	{
		if(r.IsObject()==false)
			return MakeError(NULL, -32600, "Invalid Request");

		const rapidjson::Value* pId    = JFind(r, "id");   // NULL when absent -> null id
		const bool              bHasId = pId!=NULL && pId->IsNull()==false;
		const std::string       method = JStrOf(JFind(r, "method"));

		if(method=="initialize")
		{
			const rapidjson::Value* pParams = JFind(r, "params");
			const std::string proto = pParams ? JStrOf(JFind(*pParams, "protocolVersion"), kMCPDefaultProto)
			                                  : std::string(kMCPDefaultProto);
			return MakeResult(pId, [&](JWriter& w)
			{
				w.StartObject();
				w.Key("protocolVersion"); w.String(proto);
				w.Key("capabilities");
				w.StartObject(); w.Key("tools"); w.StartObject(); w.EndObject(); w.EndObject();
				w.Key("serverInfo");
				w.StartObject();
				w.Key("name");    w.String(kMCPServerName);
				w.Key("version"); w.String(kMCPServerVersion);
				w.EndObject();
				w.EndObject();
			});
		}
		if(method=="notifications/initialized" || method=="notifications/cancelled")
			return std::string(); // notifications: no response
		if(method=="ping")
			return MakeResult(pId, [](JWriter& w){ w.StartObject(); w.EndObject(); });
		if(method=="tools/list")
		{
			// Splice the pre-serialized schema array straight in (no reparse).
			const std::string schema = cLevelEditorMCPCommands::GetToolSchemasJson();
			return MakeResult(pId, [&](JWriter& w)
			{
				w.StartObject();
				w.Key("tools"); w.RawValue(schema.data(), schema.size(), rapidjson::kArrayType);
				w.EndObject();
			});
		}
		if(method=="tools/call")
		{
			const rapidjson::Value* pParams = JFind(r, "params");
			const std::string name          = pParams ? JStrOf(JFind(*pParams, "name")) : std::string();
			const rapidjson::Value* pArgs   = pParams ? JFind(*pParams, "arguments") : NULL;

			// Re-serialize the arguments subtree for the (string-typed) queue.
			std::string sArgsJson = "{}";
			if(pArgs)
			{
				rapidjson::StringBuffer argsBuf;
				JWriter argsW(argsBuf);
				pArgs->Accept(argsW);
				sArgsJson.assign(argsBuf.GetString(), argsBuf.GetSize());
			}

			std::promise<cMCPToolResult> prom;
			std::future<cMCPToolResult> fut = prom.get_future();
			{
				std::lock_guard<std::mutex> lock(mQueueMutex);
				if(mbRunning==false)
					return MakeError(pId, -32000, "Editor is shutting down");
				cQueuedCall call;
				call.msTool     = name;
				call.msArgsJson = sArgsJson;
				call.mResult    = std::move(prom);
				mlstQueue.push_back(std::move(call));
			}

			if(fut.wait_for(std::chrono::seconds(kMCPCallTimeoutSeconds)) != std::future_status::ready)
				return MakeError(pId, -32000, "Editor did not process the call in time");

			cMCPToolResult tr = fut.get();
			// tr.msContentJson is a serialized array of content blocks (always valid
			// from our command layer); splice it in. Guard: if somehow not valid JSON,
			// wrap the raw text so the client still gets a usable block.
			rapidjson::Document check;
			check.Parse(tr.msContentJson.c_str(), tr.msContentJson.size());
			const bool bContentOk = (check.HasParseError() == false);
			return MakeResult(pId, [&](JWriter& w)
			{
				w.StartObject();
				w.Key("content");
				if(bContentOk)
					w.RawValue(tr.msContentJson.data(), tr.msContentJson.size(), rapidjson::kArrayType);
				else
				{
					const std::string block = TextContentBlock(tr.msContentJson);
					w.RawValue(block.data(), block.size(), rapidjson::kArrayType);
				}
				w.Key("isError"); w.Bool(tr.mbIsError);
				w.EndObject();
			});
		}

		if(bHasId==false) return std::string(); // unknown notification
		return MakeError(pId, -32601, "Method not found: " + method);
	};

	// MCP 2025-06-18 dropped batching, but tolerate an array of requests.
	if(reqDoc.IsArray())
	{
		std::vector<std::string> lResponses;
		for(rapidjson::Value::ConstValueIterator it=reqDoc.Begin(); it!=reqDoc.End(); ++it)
		{
			std::string resp = processOne(*it);
			if(resp.empty()==false) lResponses.push_back(std::move(resp));
		}
		if(lResponses.empty()) return std::string();

		rapidjson::StringBuffer buf;
		JWriter w(buf);
		w.StartArray();
		for(const std::string& s : lResponses)
			w.RawValue(s.data(), s.size(), rapidjson::kObjectType);
		w.EndArray();
		return std::string(buf.GetString(), buf.GetSize());
	}

	// Single request: processOne already returns "" for notifications.
	return processOne(reqDoc);
}
