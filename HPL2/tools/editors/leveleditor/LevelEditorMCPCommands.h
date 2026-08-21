/*
 * MCP tool handlers for the LevelEditor.
 *
 * Owns the tool schema table (advertised via tools/list) and the dispatch of
 * each tool to the editor. Every method here runs on the MAIN (engine) thread,
 * invoked from cLevelEditorMCPServer::DrainQueue(). Reads query the editor
 * world directly; mutations are routed through iEditorAction / AddAction() so
 * they remain undoable and keep the world "modified" flag correct.
 *
 * Kept free of the JSON library in the header (payloads cross as serialized
 * strings) so the include cost stays inside LevelEditorMCPCommands.cpp.
 */

#ifndef LEVEL_EDITOR_MCP_COMMANDS_H
#define LEVEL_EDITOR_MCP_COMMANDS_H

#include <string>

struct cMCPToolResult;

class cLevelEditor;
class iEditorWorld;
class iEntityWrapper;
class cEditorSelection;

//--------------------------------------------------------------------

class cLevelEditorMCPCommands
{
public:
	cLevelEditorMCPCommands(cLevelEditor* apEditor);

	// The tools/list payload: a serialized JSON array of
	// {name, description, inputSchema} objects. Static data, safe off-thread.
	static std::string GetToolSchemasJson();

	// Dispatch one tool call. asArgsJson is the serialized "arguments" object.
	// Returns MCP content (serialized JSON array) + isError. MAIN THREAD ONLY.
	cMCPToolResult HandleToolCall(const std::string& asTool, const std::string& asArgsJson);

private:
	cLevelEditor* mpEditor;
};

//--------------------------------------------------------------------

#endif // LEVEL_EDITOR_MCP_COMMANDS_H
