/*
 * MCP (Model Context Protocol) server for the LevelEditor.
 *
 * Exposes the editor as a set of MCP tools over a localhost HTTP/JSON-RPC 2.0
 * endpoint so an external AI agent can inspect and edit the current map.
 *
 * Threading model (critical):
 *   - cpp-httplib runs the HTTP server on its own background thread(s). Those
 *     worker threads ONLY parse JSON-RPC and touch the command queue below.
 *     They NEVER touch editor / engine / world state.
 *   - tools/call requests are marshalled onto the main (engine) thread: the
 *     worker enqueues the call and blocks on a std::future; the main thread
 *     drains the queue from cLevelEditor::OnUpdate() via DrainQueue(), runs the
 *     tool handler (building iEditorActions), and fulfils the promise.
 *
 * This header deliberately avoids including httplib.h / the JSON library so that
 * LevelEditor.cpp (which includes it) does not pay their compile cost. The JSON
 * payloads cross the server<->commands boundary as raw serialized strings.
 */

#ifndef LEVEL_EDITOR_MCP_SERVER_H
#define LEVEL_EDITOR_MCP_SERVER_H

#include <string>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <future>
#include <atomic>

namespace httplib { class Server; }

class cLevelEditor;
class cLevelEditorMCPCommands;

//--------------------------------------------------------------------

// Result of a single tool invocation. msContentJson is a serialized JSON array
// of MCP content blocks, e.g. [{"type":"text","text":"..."}].
//
// A handler may instead return a DEFERRED result (mbDeferred=true + a job id):
// the tool's real result is produced asynchronously (e.g. a multi-frame camera
// capture render + GPU readback). DrainQueue parks the caller's promise in the
// server keyed by mlDeferJobId and FulfillDeferred() completes it once the job
// finishes. The JSON payload fields are ignored for a deferred result.
struct cMCPToolResult
{
	std::string msContentJson = "[]";
	bool mbIsError = false;

	bool mbDeferred   = false;
	int  mlDeferJobId = -1;
};

//--------------------------------------------------------------------

class cLevelEditorMCPServer
{
public:
	cLevelEditorMCPServer(cLevelEditor* apEditor, const std::string& asBindAddr,
						  int alPort, const std::string& asToken);
	~cLevelEditorMCPServer();

	// Binds the socket (synchronously, so bind failures are reported here) and
	// starts the listener thread. Returns false if the port could not be bound.
	bool Start();

	// Stops the server, wakes any pending calls with an error, and joins threads.
	void Stop();

	// Called every frame on the MAIN thread (from cLevelEditor::OnUpdate). Runs
	// all queued tool handlers against the editor and fulfils their promises.
	// A handler that returns a deferred result has its promise parked (keyed by
	// job id) until FulfillDeferred() completes it.
	void DrainQueue();

	// Complete a previously-deferred tool call (MAIN thread). Called when the
	// async job (e.g. a camera capture) finishes; no-op if the id is unknown
	// (e.g. the promise was already error-fulfilled by Stop()).
	void FulfillDeferred(int alJobId, const cMCPToolResult& aResult);

	int GetPort() { return mlPort; }
	const std::string& GetBindAddr() { return msBindAddr; }
	const std::string& GetToken() { return msToken; }
	bool IsRunning() { return mbRunning; }

	// Connection-snippet catalog for the Options "MCP" tab. Static so the tab
	// can build snippets whether or not a server instance is currently running
	// (driven by the editor's stored host/port/token).
	static int         GetClientCount();
	static std::string GetClientLabel(int alIdx);
	static std::string BuildClientSnippet(int alIdx, const std::string& asHost, int alPort, const std::string& asToken);

private:
	// A tool/call marshalled from a worker thread to the main thread.
	struct cQueuedCall
	{
		std::string msTool;
		std::string msArgsJson;
		std::promise<cMCPToolResult> mResult;
	};

	// Runs on a worker thread. Parses one JSON-RPC request body and returns the
	// serialized response body (empty string for notifications -> HTTP 202).
	std::string HandleJsonRpc(const std::string& asBody);

	cLevelEditor* mpEditor;
	cLevelEditorMCPCommands* mpCommands;

	httplib::Server* mpServer;
	std::thread mListenThread;

	std::mutex mQueueMutex;
	std::deque<cQueuedCall> mlstQueue;

	// Promises for deferred tool calls, keyed by job id. Touched only on the
	// MAIN thread (DrainQueue / FulfillDeferred / Stop), so it needs no lock —
	// worker threads only ever touch mlstQueue.
	std::map<int, std::promise<cMCPToolResult>> mDeferred;

	std::string msBindAddr;
	int mlPort;
	std::string msToken;

	std::atomic<bool> mbRunning;
};

//--------------------------------------------------------------------

#endif // LEVEL_EDITOR_MCP_SERVER_H
