/*
 * MCP tool handlers for the LevelEditor. See LevelEditorMCPCommands.h.
 *
 * Everything in HandleToolCall runs on the MAIN (engine) thread. Reads query
 * the editor world directly; mutations are submitted through iEditorAction /
 * AddAction() so they stay on the undo stack.
 *
 * Tools live in ONE table (gvTools): name + description + input schema +
 * handler per entry, so the advertised schema and the dispatch can never
 * drift apart. tools/list is generated from the same table. Schemas are plain
 * JSON string literals; replies are built with RapidJSON's DOM directly
 * (allocator-threaded) and pretty-printed once at the end.
 */

// RapidJSON must be included BEFORE hpl.h: hpl.h (with VK_USE_PLATFORM_XLIB_KHR)
// pulls in X11 headers that #define Bool/True/False as macros, which would mangle
// rapidjson's Handler::Bool(...) reader methods. Parsing rapidjson first (before
// those macros exist) sidesteps the clash. (LevelEditorMCPServer.cpp avoids this
// naturally by including httplib/rapidjson ahead of any hpl headers.)
#ifndef RAPIDJSON_HAS_STDSTRING
#define RAPIDJSON_HAS_STDSTRING 1
#endif
#include "rapidjson/document.h"
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"

#include "hpl.h"
using namespace hpl;

#include "LevelEditorMCPCommands.h"
#include "LevelEditorMCPServer.h"   // cMCPToolResult
#include "LevelEditor.h"
#include "LevelEditorActions.h"
#include "LevelEditorCameraCapture.h"
#include "LevelEditorWorld.h"       // cLevelEditorEntityExtData (group ids)

#include "../common/EditorWindowViewport.h" // cEditorWindowViewport::GetCamera (defaults)

#include "../common/EditorWorld.h"
#include "../common/EditorVar.h"                   // cEditorClassInstance / cEditorVarInstance / iEditorVar (user variables)
#include "../common/EntityWrapper.h"
#include "../common/EntityWrapperEntity.h"        // eEntityStr_Filename / eEntityInt_FileIndex
#include "../common/EntityWrapperStaticObject.h"  // eStaticObjectStr_Filename / eStaticObjectInt_FileIndex
#include "../common/EditorSelection.h"
#include "../common/EditorActionHandler.h"
#include "../common/EditorAction.h"
#include "../common/EditorActionEntity.h"
#include "../common/EditorEditMode.h"
#include "../common/EditorEditModeSelect.h"
#include "../common/EditorEditModeEntities.h"

// Object-library browsing (list_object_library): the same index machinery the
// Static Objects / Entities browser windows use, so we get their cached bounds
// + triangle counts without re-scanning.
#include "../common/EditorIndex.h"                 // iEditorObjectIndex / ...Dir / ...Entry
#include "../common/EditorWindowObjectBrowser.h"   // iEditorObjectIndexEntryMeshObject (GetBVMin/Max, GetTriangleCount)
#include "../common/EditorWindowStaticObjects.h"   // cEditorObjectIndexStaticObjects (*.dae)
#include "../common/EditorWindowEntities.h"        // cEditorObjectIndexEntities (*.ent)

#include <tinyxml2.h>

#include <set>
#include <vector>

typedef rapidjson::Value                    JValue;
typedef rapidjson::Document                 JDoc;
typedef rapidjson::Document::AllocatorType  JAlloc;

//--------------------------------------------------------------------
// Argument reading — NULL-safe lookups over the parsed request object.
//--------------------------------------------------------------------

static const JValue* JFind(const JValue& aObj, const char* asKey)
{
	if(aObj.IsObject()==false) return NULL;
	JValue::ConstMemberIterator it = aObj.FindMember(asKey);
	return it!=aObj.MemberEnd() ? &it->value : NULL;
}
static bool JHas(const JValue& aObj, const char* asKey) { return JFind(aObj, asKey)!=NULL; }

static std::string JStrOf (const JValue* v, const std::string& asDef="") { return (v && v->IsString()) ? std::string(v->GetString(), v->GetStringLength()) : asDef; }

// MCP clients routinely stringify schema-untyped params (e.g. set_property's
// 'value'), so the scalar readers also coerce from string form.
static bool JParseNumber(const JValue* v, double& afOut)
{
	if(v==NULL) return false;
	if(v->IsNumber()) { afOut = v->GetDouble(); return true; }
	if(v->IsString())
	{
		const char* s = v->GetString();
		char* pEnd = NULL;
		double f = strtod(s, &pEnd);
		if(pEnd!=s) { afOut = f; return true; }
	}
	return false;
}
static int    JIntOf (const JValue* v, int alDef=0)      { double f; return JParseNumber(v, f) ? (int)f : alDef; }
static double JNumOf (const JValue* v, double afDef=0.0) { double f; return JParseNumber(v, f) ? f : afDef; }
static bool   JBoolOf(const JValue* v, bool abDef=false)
{
	if(v==NULL) return abDef;
	if(v->IsBool())   return v->GetBool();
	if(v->IsNumber()) return v->GetDouble()!=0.0;
	if(v->IsString())
	{
		tString s = cString::ToLowerCase(JStrOf(v));
		if(s=="true"  || s=="1") return true;
		if(s=="false" || s=="0") return false;
	}
	return abDef;
}

// Pull up to alMax numbers out of a stringified list ("[1, 2, 3]", "1 2 3", ...).
// Returns how many were found.
static int JParseFloatList(const JValue* v, float* apOut, int alMax)
{
	if(v==NULL || v->IsString()==false) return 0;
	const char* s = v->GetString();
	int lCount = 0;
	while(*s && lCount<alMax)
	{
		char* pEnd = NULL;
		double f = strtod(s, &pEnd);
		if(pEnd==s) { ++s; continue; }
		apOut[lCount++] = (float)f;
		s = pEnd;
	}
	return lCount;
}

static std::string JStrArg (const JValue& a, const char* k, const std::string& asDef="") { return JStrOf(JFind(a,k), asDef); }
static int         JIntArg (const JValue& a, const char* k, int alDef=0)                 { return JIntOf(JFind(a,k), alDef); }
static double      JNumArg (const JValue& a, const char* k, double afDef=0.0)            { return JNumOf(JFind(a,k), afDef); }
static bool        JBoolArg(const JValue& a, const char* k, bool abDef=false)            { return JBoolOf(JFind(a,k), abDef); }

static cVector3f JVec3Of(const JValue* v, const cVector3f& aDef)
{
	if(v && v->IsArray() && v->Size()>=3 && (*v)[0].IsNumber() && (*v)[1].IsNumber() && (*v)[2].IsNumber())
		return cVector3f((float)(*v)[0].GetDouble(), (float)(*v)[1].GetDouble(), (float)(*v)[2].GetDouble());
	if(v && v->IsObject())
		return cVector3f((float)JNumOf(JFind(*v,"x"), aDef.x), (float)JNumOf(JFind(*v,"y"), aDef.y), (float)JNumOf(JFind(*v,"z"), aDef.z));
	float vVals[3];
	if(JParseFloatList(v, vVals, 3)==3)
		return cVector3f(vVals[0], vVals[1], vVals[2]);
	return aDef;
}
static cVector2f JVec2Of(const JValue* v, const cVector2f& aDef)
{
	if(v && v->IsArray() && v->Size()>=2 && (*v)[0].IsNumber() && (*v)[1].IsNumber())
		return cVector2f((float)(*v)[0].GetDouble(), (float)(*v)[1].GetDouble());
	float vVals[2];
	if(JParseFloatList(v, vVals, 2)==2)
		return cVector2f(vVals[0], vVals[1]);
	return aDef;
}
static cColor JColorOf(const JValue* v, const cColor& aDef)
{
	if(v && v->IsArray() && v->Size()>=3 && (*v)[0].IsNumber() && (*v)[1].IsNumber() && (*v)[2].IsNumber())
	{
		float a = (v->Size()>=4 && (*v)[3].IsNumber()) ? (float)(*v)[3].GetDouble() : 1.0f;
		return cColor((float)(*v)[0].GetDouble(), (float)(*v)[1].GetDouble(), (float)(*v)[2].GetDouble(), a);
	}
	float vVals[4];
	int lCount = JParseFloatList(v, vVals, 4);
	if(lCount>=3)
		return cColor(vVals[0], vVals[1], vVals[2], lCount>=4 ? vVals[3] : 1.0f);
	return aDef;
}

// Collect entity IDs from either "ids":[..] or a single "id".
static tIntList ParseIds(const JValue& a)
{
	tIntList ids;
	const JValue* pArr = JFind(a, "ids");
	if(pArr && pArr->IsArray())
		for(JValue::ConstValueIterator it=pArr->Begin(); it!=pArr->End(); ++it)
			if(it->IsInt()) ids.push_back(it->GetInt());
	const JValue* pId = JFind(a, "id");
	if(pId && pId->IsInt()) ids.push_back(pId->GetInt());
	return ids;
}

static iEntityWrapper* ResolveEntity(iEditorWorld* apWorld, const JValue& a)
{
	const JValue* pId = JFind(a, "id");
	if(pId && pId->IsInt())
		return apWorld->GetEntity(pId->GetInt());
	const JValue* pName = JFind(a, "name");
	if(pName && pName->IsString())
		return apWorld->GetEntityByName(JStrOf(pName));
	return NULL;
}

// Resolve id / ids / name into wrappers. Unknown ids are reported in asErr.
static std::vector<iEntityWrapper*> ResolveEntities(iEditorWorld* apWorld, const JValue& a, std::string& asErr)
{
	std::vector<iEntityWrapper*> vEnts;
	tIntList ids = ParseIds(a);
	for(tIntListIt it=ids.begin(); it!=ids.end(); ++it)
	{
		iEntityWrapper* e = apWorld->GetEntity(*it);
		if(e==NULL) { asErr = "entity id " + std::to_string(*it) + " not found"; return std::vector<iEntityWrapper*>(); }
		vEnts.push_back(e);
	}
	const JValue* pName = JFind(a, "name");
	if(vEnts.empty() && pName && pName->IsString())
	{
		iEntityWrapper* e = apWorld->GetEntityByName(JStrOf(pName));
		if(e==NULL) { asErr = "entity '" + JStrOf(pName) + "' not found"; return vEnts; }
		vEnts.push_back(e);
	}
	if(vEnts.empty() && asErr.empty()) asErr = "no target entity (pass 'id', 'ids' or 'name')";
	return vEnts;
}

//--------------------------------------------------------------------
// Reply building — engine types to rapidjson values.
//--------------------------------------------------------------------

static JValue JVec3(const cVector3f& v, JAlloc& a)
{
	JValue j(rapidjson::kArrayType);
	j.PushBack(v.x, a).PushBack(v.y, a).PushBack(v.z, a);
	return j;
}
static JValue JVec2(const cVector2f& v, JAlloc& a)
{
	JValue j(rapidjson::kArrayType);
	j.PushBack(v.x, a).PushBack(v.y, a);
	return j;
}
static JValue JColor(const cColor& c, JAlloc& a)
{
	JValue j(rapidjson::kArrayType);
	j.PushBack(c.r, a).PushBack(c.g, a).PushBack(c.b, a).PushBack(c.a, a);
	return j;
}
static JValue JBounds(const cVector3f& avMin, const cVector3f& avMax, JAlloc& a)
{
	JValue j(rapidjson::kObjectType);
	j.AddMember("min",    JVec3(avMin, a), a);
	j.AddMember("max",    JVec3(avMax, a), a);
	j.AddMember("size",   JVec3(avMax - avMin, a), a);
	j.AddMember("center", JVec3((avMin + avMax) * 0.5f, a), a);
	return j;
}

//--------------------------------------------------------------------
// Property plumbing (shared by get/set/list property tools and create)
//--------------------------------------------------------------------

static const char* PropTypeName(eVariableType aType)
{
	switch(aType)
	{
		case eVariableType_Int:    return "int";
		case eVariableType_Float:  return "float";
		case eVariableType_Bool:   return "bool";
		case eVariableType_String: return "string";
		case eVariableType_Vec2:   return "vec2 [x,y]";
		case eVariableType_Vec3:   return "vec3 [x,y,z]";
		case eVariableType_Color:  return "color [r,g,b,a]";
		case eVariableType_Enum:   return "enum";
		default:                   return "?";
	}
}

// Read one typed property off an instance into aObj[asKey]. False if unsupported.
static bool AddPropValue(JValue& aObj, const char* asKey, iEntityWrapper* e, iProp* p, JAlloc& a)
{
	int pid = p->GetID();
	rapidjson::GenericStringRef<char> key(asKey);
	switch(p->GetType())
	{
		case eVariableType_Int:    { int v=0;      e->GetProperty(pid,v); aObj.AddMember(key, v, a); return true; }
		case eVariableType_Float:  { float v=0;    e->GetProperty(pid,v); aObj.AddMember(key, v, a); return true; }
		case eVariableType_Bool:   { bool v=false; e->GetProperty(pid,v); aObj.AddMember(key, v, a); return true; }
		case eVariableType_String: { tString v;    e->GetProperty(pid,v); aObj.AddMember(key, JValue(v, a), a); return true; }
		case eVariableType_Vec2:   { cVector2f v;  e->GetProperty(pid,v); aObj.AddMember(key, JVec2(v, a), a); return true; }
		case eVariableType_Vec3:   { cVector3f v;  e->GetProperty(pid,v); aObj.AddMember(key, JVec3(v, a), a); return true; }
		case eVariableType_Color:  { cColor v;     e->GetProperty(pid,v); aObj.AddMember(key, JColor(v, a), a); return true; }
		default: return false;
	}
}

// True if the property already holds the requested value (a no-op). Lets the
// handler skip a set the undo system would reject as an "invalid action" (and
// the ERROR it logs), so re-setting a property to its current value succeeds.
static bool PropEqualsCurrent(iEntityWrapper* e, iProp* p, const JValue* val)
{
	int pid = p->GetID();
	switch(p->GetType())
	{
		case eVariableType_Int:    { int v=0;        e->GetProperty(pid,v); return v==JIntOf(val); }
		case eVariableType_Float:  { float v=0;      e->GetProperty(pid,v); return v==(float)JNumOf(val); }
		case eVariableType_Bool:   { bool v=false;   e->GetProperty(pid,v); return v==JBoolOf(val); }
		case eVariableType_String: { tString v;      e->GetProperty(pid,v); return v==JStrOf(val); }
		case eVariableType_Vec2:   { cVector2f v(0); e->GetProperty(pid,v); return v==JVec2Of(val, cVector2f(0)); }
		case eVariableType_Vec3:   { cVector3f v(0); e->GetProperty(pid,v); return v==JVec3Of(val, cVector3f(0)); }
		case eVariableType_Color:  { cColor v;       e->GetProperty(pid,v); return v==JColorOf(val, cColor(1)); }
		default: return false;
	}
}

// Typed set-property action from a JSON value. NULL + message on unsupported type.
static iEditorAction* MakePropSetAction(iEntityWrapper* e, iProp* p, const JValue* val, std::string& asErr)
{
	int pid = p->GetID();
	switch(p->GetType())
	{
		case eVariableType_Int:    return e->CreateSetPropertyActionInt(pid, JIntOf(val));
		case eVariableType_Float:  return e->CreateSetPropertyActionFloat(pid, (float)JNumOf(val));
		case eVariableType_Bool:   return e->CreateSetPropertyActionBool(pid, JBoolOf(val));
		case eVariableType_String: return e->CreateSetPropertyActionString(pid, JStrOf(val));
		case eVariableType_Vec2:   return e->CreateSetPropertyActionVector2f(pid, JVec2Of(val, cVector2f(0)));
		case eVariableType_Vec3:   return e->CreateSetPropertyActionVector3f(pid, JVec3Of(val, cVector3f(0)));
		case eVariableType_Color:  return e->CreateSetPropertyActionColor(pid, JColorOf(val, cColor(1)));
		default: asErr = "unsupported property type on '" + tString(p->GetName()) + "'"; return NULL;
	}
}

// Apply a property value onto not-yet-created entity data (create_entity 'properties').
static bool ApplyPropToData(iEntityWrapperType* pType, iEntityWrapperData* pData,
                            const std::string& asName, const JValue* val, std::string& asErr)
{
	iProp* p = pType->GetPropByName(asName);
	if(p==NULL) { asErr = "no such property: " + asName + " (see list_properties)"; return false; }
	int pid = p->GetID();
	switch(p->GetType())
	{
		case eVariableType_Int:    pData->SetInt(pid, JIntOf(val)); break;
		case eVariableType_Float:  pData->SetFloat(pid, (float)JNumOf(val)); break;
		case eVariableType_Bool:   pData->SetBool(pid, JBoolOf(val)); break;
		case eVariableType_String: pData->SetString(pid, JStrOf(val)); break;
		case eVariableType_Vec2:   pData->SetVec2f(pid, JVec2Of(val, cVector2f(0))); break;
		case eVariableType_Vec3:   pData->SetVec3f(pid, JVec3Of(val, cVector3f(0))); break;
		case eVariableType_Color:  pData->SetColor(pid, JColorOf(val, cColor(1))); break;
		default: asErr = "unsupported property type on '" + asName + "'"; return false;
	}
	return true;
}

//--------------------------------------------------------------------
// User / script variable plumbing (per-instance <UserVariables>)
//--------------------------------------------------------------------

// User variables are stored as untyped strings (the engine parses them per the
// declared var type). Coerce a JSON value to that string: strings pass through
// verbatim; scalars stringify; arrays (vec/color vars) join with spaces.
// For anything ambiguous the agent should pass the string exactly as it appears
// in get_entity's XML / list_properties.
static std::string JValueToVarString(const JValue* v)
{
	if(v==NULL) return "";
	if(v->IsString()) return std::string(v->GetString(), v->GetStringLength());
	if(v->IsBool())   return v->GetBool() ? "true" : "false";
	if(v->IsInt())    return std::to_string(v->GetInt());
	if(v->IsUint())   return std::to_string(v->GetUint());
	if(v->IsNumber()) { char b[32]; snprintf(b, sizeof(b), "%g", v->GetDouble()); return std::string(b); }
	if(v->IsArray())
	{
		std::string s;
		for(rapidjson::SizeType i=0;i<v->Size();++i)
		{
			if(i) s += " ";
			const JValue& e = (*v)[i];
			if(e.IsNumber())      { char b[32]; snprintf(b, sizeof(b), "%g", e.GetDouble()); s += b; }
			else if(e.IsString()) s += std::string(e.GetString(), e.GetStringLength());
		}
		return s;
	}
	return "";
}

// Only entities of a user-defined type carry per-instance variables. The engine
// itself never checks (it C-casts from static context); here the target entity
// comes from an arbitrary agent request, so use a checked cross-cast (RTTI is on;
// iEntityWrapper is polymorphic). NULL for non-user-defined entities (lights,
// areas, primitives, ...).
static iEntityWrapperUserDefinedEntity* AsUserDefined(iEntityWrapper* e)
{
	return dynamic_cast<iEntityWrapperUserDefinedEntity*>(e);
}

// Build one undoable CreateActionSetUserVariable per (entity, var). Validates
// every entity is user-defined and every named var exists BEFORE building any
// action; on failure fills asErr and frees what it built. Shared by set_variable
// and edit_entity.
static bool BuildVarSetActions(const std::vector<iEntityWrapper*>& avEnts,
                               const std::vector<std::pair<std::string, const JValue*>>& avVars,
                               std::vector<iEditorAction*>& avActions, std::string& asErr)
{
	for(size_t i=0;i<avEnts.size();++i)
	{
		iEntityWrapperUserDefinedEntity* pU = AsUserDefined(avEnts[i]);
		if(pU==NULL)
		{
			asErr = "entity " + std::to_string(avEnts[i]->GetID()) + " ('" +
			        tString(avEnts[i]->GetName()) + "') has no user variables";
			return false;
		}
		for(size_t k=0;k<avVars.size();++k)
		{
			tWString wName = cString::To16Char(avVars[k].first);
			if(pU->GetVar(wName)==NULL)
			{
				asErr = "no such variable '" + avVars[k].first + "' on entity " +
				        std::to_string(avEnts[i]->GetID()) + " (see list_properties)";
				return false;
			}
			tWString wVal = cString::To16Char(JValueToVarString(avVars[k].second));
			iEditorAction* pAction = pU->CreateActionSetUserVariable(wName, wVal);
			if(pAction) avActions.push_back(pAction);
		}
	}
	return true;
}

// Collect (name, value) pairs from a 'variables':{...} object and/or a single
// 'variable' + 'value'.
static std::vector<std::pair<std::string, const JValue*>> CollectVarPairs(const JValue& aArgs)
{
	std::vector<std::pair<std::string, const JValue*>> vVars;
	const JValue* pVars = JFind(aArgs, "variables");
	if(pVars && pVars->IsObject())
		for(JValue::ConstMemberIterator it=pVars->MemberBegin(); it!=pVars->MemberEnd(); ++it)
			vVars.push_back(std::make_pair(std::string(it->name.GetString()), &it->value));
	std::string sSingle = JStrArg(aArgs, "variable");
	if(sSingle.empty()==false)
		vVars.push_back(std::make_pair(sSingle, JFind(aArgs, "value")));
	return vVars;
}

//--------------------------------------------------------------------
// Entity JSON views
//--------------------------------------------------------------------

// The entity's native XML descriptor — exactly what the engine saves/loads for
// it (all typed properties + user variables). Serialized via the entity's own
// data->Save() into a throwaway document. We use CreateCopyData()+Save() rather
// than iEntityWrapper::Save() to avoid the latter's out-of-bounds "outlier"
// side effect on the world.
static void AddEntityXml(JValue& j, iEntityWrapper* e, JAlloc& a)
{
	tinyxml2::XMLDocument doc;
	tinyxml2::XMLElement* pRoot = doc.NewElement("root");
	doc.InsertEndChild(pRoot);

	iEntityWrapperData* pData = e->CreateCopyData();
	const bool bOk = (pData != NULL) && pData->Save(pRoot);
	if(pData) hplDelete(pData);

	tinyxml2::XMLElement* pEl = pRoot->FirstChildElement();
	if(bOk && pEl)
	{
		tinyxml2::XMLPrinter printer;
		pEl->Accept(&printer);
		j.AddMember("xml", JValue(printer.CStr(), a), a);
	}
	else
		j.AddMember("xml", JValue(rapidjson::kNullType), a);
}

// Minimal overview row: id, name, type only. Used by query_entities detail:'summary'.
static JValue EntitySummary(iEntityWrapper* e, JAlloc& a)
{
	JValue j(rapidjson::kObjectType);
	j.AddMember("id",   e->GetID(), a);
	j.AddMember("name", JValue(e->GetName(), a), a);
	j.AddMember("type", JValue(cString::To8Char(e->GetTypeName()), a), a);
	return j;
}

enum eRegionMode { eRegionMode_Intersect, eRegionMode_Inside, eRegionMode_Center };

// True if entity e falls within the world-AABB [avMin,avMax] under the given mode.
// Falls back to an origin-in-box test whenever the entity has no render BV (lights/sound/particles).
static bool EntityInRegion(iEntityWrapper* e, const cVector3f& avMin, const cVector3f& avMax, int aMode)
{
	cBoundingVolume* pBV = (aMode==eRegionMode_Center) ? NULL : e->GetRenderBV();
	if(pBV)
	{
		if(aMode==eRegionMode_Inside)
			return cMath::CheckAABBInside(pBV->GetMin(), pBV->GetMax(), avMin, avMax);
		return cMath::CheckAABBIntersection(avMin, avMax, pBV->GetMin(), pBV->GetMax());
	}
	return cMath::CheckPointInAABBIntersection(e->GetPosition(), avMin, avMax);
}

// Compact list-friendly row.
static JValue EntityCompact(cLevelEditor* pEditor, iEntityWrapper* e, bool abBounds, JAlloc& a)
{
	JValue j(rapidjson::kObjectType);
	j.AddMember("id",       e->GetID(), a);
	j.AddMember("name",     JValue(e->GetName(), a), a);
	j.AddMember("type",     JValue(cString::To8Char(e->GetTypeName()), a), a);
	j.AddMember("position", JVec3(e->GetPosition(), a), a);
	j.AddMember("rotation", JVec3(e->GetRotation(), a), a);
	j.AddMember("scale",    JVec3(e->GetScale(), a), a);

	if(pEditor->GetSelection()->HasEntity(e->GetID()))
		j.AddMember("selected", true, a);

	cLevelEditorEntityExtData* pExt = (cLevelEditorEntityExtData*)e->GetEntityExtData();
	if(pExt && pExt->mlGroupID!=0)
		j.AddMember("group", pExt->mlGroupID, a);

	// Source file, for types that carry one (Entity/Static Object/PS/Sound/Billboard).
	static const char* kFileProps[] = { "Filename", "File", "SoundEntityFile", "MaterialFile" };
	for(int i=0;i<4;++i)
	{
		iProp* p = e->GetType()->GetPropByName(kFileProps[i]);
		if(p && p->GetType()==eVariableType_String)
		{
			tString s; e->GetProperty(p->GetID(), s);
			if(s.empty()==false) { j.AddMember("file", JValue(s, a), a); break; }
		}
	}

	if(abBounds)
	{
		cBoundingVolume* pBV = e->GetRenderBV();
		if(pBV) j.AddMember("bounds", JBounds(pBV->GetMin(), pBV->GetMax(), a), a);
	}
	return j;
}

//--------------------------------------------------------------------
// Result builders
//--------------------------------------------------------------------

static cMCPToolResult MakeText(const std::string& s, bool abError=false)
{
	rapidjson::StringBuffer buf;
	rapidjson::Writer<rapidjson::StringBuffer> w(buf);
	w.StartArray();
	w.StartObject();
	w.Key("type"); w.String("text");
	w.Key("text"); w.String(s);
	w.EndObject();
	w.EndArray();

	cMCPToolResult r;
	r.mbIsError = abError;
	r.msContentJson = std::string(buf.GetString(), buf.GetSize());
	return r;
}

// Serialize a reply document into a single compact text content block. MCP replies are
// machine-parsed, so we skip pretty-printing to avoid the per-line token overhead.
static cMCPToolResult MakeDoc(const JDoc& d, bool abError=false)
{
	rapidjson::StringBuffer buf;
	rapidjson::Writer<rapidjson::StringBuffer> w(buf);
	d.Accept(w);
	return MakeText(std::string(buf.GetString(), buf.GetSize()), abError);
}

static cMCPToolResult MakeErr(const std::string& asMsg)
{
	JDoc d; d.SetObject();
	d.AddMember("ok", false, d.GetAllocator());
	d.AddMember("error", JValue(asMsg, d.GetAllocator()), d.GetAllocator());
	return MakeDoc(d, true);
}

// Mutation replies carry "ok":true first; add payload members to the returned doc.
static void InitOk(JDoc& d) { d.SetObject(); d.AddMember("ok", true, d.GetAllocator()); }
static cMCPToolResult MakeOk() { JDoc d; InitOk(d); return MakeDoc(d); }

//--------------------------------------------------------------------
// Create plumbing (create_entity / create_entities)
//--------------------------------------------------------------------

// 'type' resolves against the wrapper display name first ("Light", "Entity",
// "Static Object", ...), then against the XML element name so subtypes like
// "SpotLight" / "AreaLight" are reachable ("Light" alone = point light).
static iEntityWrapperType* ResolveCreateType(cLevelEditor* pEditor, iEditorWorld* pWorld, const JValue& aSpec)
{
	std::string sType = JStrArg(aSpec, "type");
	std::string sFile = JStrArg(aSpec, "file");

	const JValue* pTypeId = JFind(aSpec, "typeId");
	if(pTypeId && pTypeId->IsInt())
		return pWorld->GetEntityTypeByID(pTypeId->GetInt());
	if(sType.empty())
		return NULL;

	// Entity subtype is resolved from the .ent file itself.
	if(sType=="Entity" && sFile.empty()==false)
	{
		cEditorEditModeEntities* pEM = (cEditorEditModeEntities*)pEditor->GetEditMode("Entities");
		iEntityWrapperType* pType = pEM ? pEM->GetTypeFromEntFile(sFile) : NULL;
		if(pType) return pType;
	}

	iEntityWrapperType* pType = pWorld->GetEntityTypeByName(sType);
	if(pType) return pType;

	int n = pWorld->GetEntityTypeNum();
	for(int i=0;i<n;++i)
	{
		iEntityWrapperType* t = pWorld->GetEntityType(i);
		if(t && t->GetXmlElementName()==sType) return t;
	}
	return NULL;
}

// Unique name: like iEditorWorld::GenerateName, but also avoiding names already
// handed out in this call (batch creates run after all names are generated).
static tString MakeUniqueName(iEditorWorld* pWorld, const tString& asBase, std::set<tString>& asetUsed)
{
	tString sName = pWorld->GenerateName(asBase);
	int lSuffix = 1;
	while(asetUsed.count(sName) > 0 || pWorld->IsNameAvailable(sName)==false)
		sName = asBase + "_" + cString::ToString(lSuffix++);
	asetUsed.insert(sName);
	return sName;
}

// Validate one create spec and build its (not yet executed) create action.
// On success returns the action and fills id; on failure returns NULL with asErr.
static iEditorAction* BuildCreateAction(cLevelEditor* pEditor, iEditorWorld* pWorld, const JValue& aSpec,
                                        std::set<tString>& asetUsedNames, int* apOutID, std::string& asErr)
{
	iEntityWrapperType* pType = ResolveCreateType(pEditor, pWorld, aSpec);
	if(pType==NULL)
	{
		asErr = "unknown entity type (pass a 'type' name or xmlName, or 'typeId'; see list_entity_types)";
		return NULL;
	}

	std::string sFile   = JStrArg(aSpec, "file");
	int lTypeID         = pType->GetID();
	const bool bIsEnt   = (lTypeID==eEditorEntityType_Entity);
	const bool bIsSObj  = (lTypeID==eEditorEntityType_StaticObject);

	if((bIsEnt || bIsSObj) && sFile.empty())
	{
		asErr = "type '" + cString::To8Char(pType->GetName()) + "' requires a 'file' argument";
		return NULL;
	}

	iEntityWrapperData* pData = pType->CreateData();
	if(pData==NULL) { asErr = "failed to create data for type"; return NULL; }

	pData->SetVec3f(eObjVec3f_Position, JVec3Of(JFind(aSpec,"position"), cVector3f(0)));
	pData->SetVec3f(eObjVec3f_Rotation, JVec3Of(JFind(aSpec,"rotation"), cVector3f(0)));
	pData->SetVec3f(eObjVec3f_Scale,    JVec3Of(JFind(aSpec,"scale"),    cVector3f(1)));

	if(sFile.empty()==false)
	{
		if(bIsSObj)
		{
			pData->SetString(eStaticObjectStr_Filename, sFile);
			pData->SetInt(eStaticObjectInt_FileIndex, pWorld->AddFilenameToIndex("StaticObjects", sFile));
		}
		else if(bIsEnt)
		{
			pData->SetString(eEntityStr_Filename, sFile);
			pData->SetInt(eEntityInt_FileIndex, pWorld->AddFilenameToIndex("Entities", sFile));
		}
		else
		{
			// Route 'file' to the type's file-ish string property
			// (Particle System: File, Sound: SoundEntityFile, Billboard: MaterialFile).
			static const char* kFileProps[] = { "File", "SoundEntityFile", "MaterialFile" };
			iProp* pFileProp = NULL;
			for(int i=0;i<3 && pFileProp==NULL;++i)
				pFileProp = pType->GetPropByName(kFileProps[i]);
			if(pFileProp==NULL || pFileProp->GetType()!=eVariableType_String)
			{
				hplDelete(pData);
				asErr = "type '" + cString::To8Char(pType->GetName()) + "' does not take a 'file'";
				return NULL;
			}
			pData->SetString(pFileProp->GetID(), sFile);
		}
	}

	// Extra typed properties, applied pre-create: the object is born configured
	// and the whole create stays ONE undo step.
	const JValue* pProps = JFind(aSpec, "properties");
	if(pProps && pProps->IsObject())
	{
		for(JValue::ConstMemberIterator it=pProps->MemberBegin(); it!=pProps->MemberEnd(); ++it)
		{
			if(ApplyPropToData(pType, pData, it->name.GetString(), &it->value, asErr)==false)
			{
				hplDelete(pData);
				return NULL;
			}
		}
	}

	pData->PostCreateSetUp();

	std::string sBase = JStrArg(aSpec, "name", cString::To8Char(pType->GetName()));
	int lNewID = pWorld->GetFreeID();
	pData->SetID(lNewID);
	pData->SetName(MakeUniqueName(pWorld, sBase, asetUsedNames));

	if(apOutID) *apOutID = lNewID;
	return hplNew(cEditorActionObjectCreate, (pWorld, pData));
}

//--------------------------------------------------------------------
// Tool registry
//--------------------------------------------------------------------

struct cMCPToolCtx
{
	cLevelEditor* mpEditor;
	iEditorWorld* mpWorld;
	const JValue& margs;      // always an object
};

typedef cMCPToolResult (*tMCPToolHandlerFn)(cMCPToolCtx&);

struct cMCPToolDef
{
	const char*       msName;
	const char*       msDesc;
	const char*       msSchemaJson;   // inputSchema as a JSON literal
	tMCPToolHandlerFn mpfHandler;
};

// Shared schema fragment for create_entity / items of create_entities
// (adjacent-literal concatenation).
#define MCP_CREATE_SPEC_SCHEMA \
	"{\"type\":\"object\",\"properties\":{" \
	"\"type\":{\"type\":\"string\",\"description\":\"type name or xmlName (see list_entity_types), e.g. 'Light', 'SpotLight', 'Entity', 'Static Object', 'Particle System'\"}," \
	"\"typeId\":{\"type\":\"integer\",\"description\":\"alternative to 'type'\"}," \
	"\"file\":{\"type\":\"string\",\"description\":\"source file, resource-relative: .ent for Entity, .dae/.msh for Static Object, .ps for Particle System, .snt for Sound, .mat for Billboard\"}," \
	"\"name\":{\"type\":\"string\",\"description\":\"base name (auto-uniquified)\"}," \
	"\"position\":{\"type\":\"array\",\"items\":{\"type\":\"number\"},\"minItems\":3,\"maxItems\":3,\"description\":\"world position [x,y,z]\"}," \
	"\"rotation\":{\"type\":\"array\",\"items\":{\"type\":\"number\"},\"minItems\":3,\"maxItems\":3,\"description\":\"rotation [x,y,z] radians\"}," \
	"\"scale\":{\"type\":\"array\",\"items\":{\"type\":\"number\"},\"minItems\":3,\"maxItems\":3,\"description\":\"scale [x,y,z]\"}," \
	"\"properties\":{\"type\":\"object\",\"description\":\"typed properties to set at creation, e.g. {\\\"Radius\\\": 9, \\\"DiffuseColor\\\": [1,0.7,0.4,1], \\\"CastShadows\\\": true} — names per list_properties\"}" \
	"}}"

//--------------------------------------------------------------------
// Handlers that are shared between tools
//--------------------------------------------------------------------

static cMCPToolResult SelectImpl(cMCPToolCtx& c, const char* asWhich)
{
	cEditorEditModeSelect* pSel = (cEditorEditModeSelect*)c.mpEditor->GetEditMode("Select");
	if(tString(asWhich)=="clear")
	{
		c.mpEditor->AddAction(pSel->CreateSelectEntityAction(tIntList(), eSelectActionType_Clear));
		return MakeOk();
	}
	tIntList ids = ParseIds(c.margs);
	if(ids.empty()) return MakeErr("no ids given");
	if(tString(asWhich)=="deselect")
	{
		c.mpEditor->AddAction(pSel->CreateSelectEntityAction(ids, eSelectActionType_Deselect));
		return MakeOk();
	}
	std::string sMode = JStrArg(c.margs, "mode", "add");
	if(sMode=="set")
	{
		c.mpEditor->AddAction(pSel->CreateSelectEntityAction(tIntList(), eSelectActionType_Clear));
		c.mpEditor->AddAction(pSel->CreateSelectEntityAction(ids, eSelectActionType_Select));
	}
	else if(sMode=="toggle")
		c.mpEditor->AddAction(pSel->CreateSelectEntityAction(ids, eSelectActionType_Toggle));
	else // "add"
		c.mpEditor->AddAction(pSel->CreateSelectEntityAction(ids, eSelectActionType_Select));
	return MakeOk();
}

static cMCPToolResult CreateEntitiesImpl(cMCPToolCtx& c, bool abBatch)
{
	// Collect specs.
	std::vector<const JValue*> vSpecs;
	if(abBatch)
	{
		const JValue* pArr = JFind(c.margs, "entities");
		if(pArr==NULL || pArr->IsArray()==false || pArr->Size()==0)
			return MakeErr("pass 'entities': [spec, ...] (each spec like create_entity's arguments)");
		for(JValue::ConstValueIterator it=pArr->Begin(); it!=pArr->End(); ++it)
			vSpecs.push_back(&*it);
	}
	else
		vSpecs.push_back(&c.margs);

	// Validate + build ALL actions before executing any, so a bad spec fails
	// the whole call without side effects.
	std::vector<iEditorAction*> vActions;
	std::vector<int> vIDs;
	std::set<tString> setNames;
	for(size_t i=0;i<vSpecs.size();++i)
	{
		int lID = -1;
		std::string sErr;
		iEditorAction* pAction = BuildCreateAction(c.mpEditor, c.mpWorld, *vSpecs[i], setNames, &lID, sErr);
		if(pAction==NULL)
		{
			for(size_t k=0;k<vActions.size();++k) hplDelete(vActions[k]);
			return MakeErr(abBatch ? ("entities[" + std::to_string(i) + "]: " + sErr) : sErr);
		}
		vActions.push_back(pAction);
		vIDs.push_back(lID);
	}

	// One undo step for the whole batch.
	if(vActions.size()==1)
		c.mpEditor->AddAction(vActions[0]);
	else
	{
		cEditorActionCompoundAction* pCompound = hplNew(cEditorActionCompoundAction, ("MCP create_entities"));
		for(size_t i=0;i<vActions.size();++i) pCompound->AddAction(vActions[i]);
		c.mpEditor->AddAction(pCompound);
	}

	JDoc d; InitOk(d);
	JAlloc& a = d.GetAllocator();
	if(abBatch==false)
	{
		iEntityWrapper* e = c.mpWorld->GetEntity(vIDs[0]);
		if(e==NULL) return MakeErr("create action ran but no entity was produced (bad file/type?)");
		d.AddMember("entity", EntityCompact(c.mpEditor, e, false, a), a);
		return MakeDoc(d);
	}
	JValue arr(rapidjson::kArrayType);
	for(size_t i=0;i<vIDs.size();++i)
	{
		iEntityWrapper* e = c.mpWorld->GetEntity(vIDs[i]);
		if(e) arr.PushBack(EntityCompact(c.mpEditor, e, false, a), a);
		else  arr.PushBack(JValue(rapidjson::kNullType), a);
	}
	d.AddMember("count", (int)arr.Size(), a);
	d.AddMember("created", arr, a);
	return MakeDoc(d);
}

//--------------------------------------------------------------------
// Object-library + mesh helpers (list_object_library / get_mesh_data)
//--------------------------------------------------------------------

// Recursively walk an object index's dir tree, appending matching entries to
// arrOut. Mirrors cEditorWindowObjectBrowser::AddEntriesInDirToList. Entries in
// the static/entity indices are always iEditorObjectIndexEntryMeshObject, whose
// AABB + triangle count come from the on-disk .sop/.enp cache.
static void MCPCollectIndexEntries(iEditorObjectIndexDir* apDir, const char* asType,
                                   const std::string& asCatFilter, const std::string& asQuery,
                                   bool abBounds, int alLimit,
                                   JValue& arrOut, JAlloc& a,
                                   std::set<std::string>& asetCats, int& alTotal)
{
	if(apDir==NULL) return;

	const std::string sCat      = cString::To8Char(apDir->GetRelPath());
	const std::string sCatLower = cString::ToLowerCase(sCat);
	const bool bCatMatch = asCatFilter.empty() || sCatLower.find(asCatFilter)!=std::string::npos;

	if(bCatMatch)
	{
		tIndexEntryMap& mapEntries = apDir->GetEntries();
		for(tIndexEntryMapConstIt it=mapEntries.begin(); it!=mapEntries.end(); ++it)
		{
			iEditorObjectIndexEntry* pEntry = it->second;
			if(pEntry==NULL) continue;

			const std::string sName = pEntry->GetEntryName();
			const std::string sPath = cString::To8Char(pEntry->GetFileNameFullPath());

			if(asQuery.empty()==false &&
			   cString::ToLowerCase(sName).find(asQuery)==std::string::npos &&
			   cString::ToLowerCase(sPath).find(asQuery)==std::string::npos)
				continue;

			asetCats.insert(sCat);
			++alTotal;
			if((int)arrOut.Size()>=alLimit) continue; // counted in totalMatches, but capped

			JValue j(rapidjson::kObjectType);
			j.AddMember("name",     JValue(sName, a), a);
			j.AddMember("file",     JValue(sPath, a), a);
			j.AddMember("relPath",  JValue(cString::To8Char(pEntry->GetFileNameRelPath()), a), a);
			j.AddMember("category", JValue(sCat, a), a);
			j.AddMember("type",     JValue(asType, a), a);

			iEditorObjectIndexEntryMeshObject* pMesh = (iEditorObjectIndexEntryMeshObject*)pEntry;
			j.AddMember("triangleCount", pMesh->GetTriangleCount(), a);
			if(abBounds)
			{
				const cVector3f& vMin = pMesh->GetBVMin();
				const cVector3f& vMax = pMesh->GetBVMax();
				if(vMax.x>=vMin.x && vMax.y>=vMin.y && vMax.z>=vMin.z)
					j.AddMember("bounds", JBounds(vMin, vMax, a), a);
			}
			arrOut.PushBack(j, a);
		}
	}

	tIndexDirMap& mapSubDirs = (tIndexDirMap&)apDir->GetSubDirs();
	for(tIndexDirMapIt it=mapSubDirs.begin(); it!=mapSubDirs.end(); ++it)
		MCPCollectIndexEntries(it->second, asType, asCatFilter, asQuery, abBounds, alLimit, arrOut, a, asetCats, alTotal);
}

// Minimal RFC-4648 base64 (kept local, like LevelEditorCameraCapture's own copy,
// to avoid cross-TU coupling). Used by get_mesh_data's 'base64' format.
static std::string MCPBase64(const unsigned char* apData, size_t alLen)
{
	static const char kTbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string sOut;
	sOut.reserve(((alLen+2)/3)*4);
	for(size_t i=0;i<alLen;i+=3)
	{
		unsigned int n = (unsigned int)apData[i] << 16;
		if(i+1<alLen) n |= (unsigned int)apData[i+1] << 8;
		if(i+2<alLen) n |= (unsigned int)apData[i+2];
		sOut.push_back(kTbl[(n>>18)&63]);
		sOut.push_back(kTbl[(n>>12)&63]);
		sOut.push_back((i+1<alLen) ? kTbl[(n>>6)&63] : '=');
		sOut.push_back((i+2<alLen) ? kTbl[n&63]      : '=');
	}
	return sOut;
}

//--------------------------------------------------------------------
// The tool table
//--------------------------------------------------------------------

static const cMCPToolDef gvTools[] =
{

// ----- reads -----

{ "query_entities",
  "Query objects in the current map as compact rows {id, name, type, position, rotation, scale, selected?, group?, file?}. "
  "Filters (AND-combined): 'type' (exact type name, e.g. 'Light'), 'name' (substring), 'region' (world-AABB). "
  "Page with 'limit' (default 200) + 'offset'; reply carries 'count', 'totalMatches', and (when more remain) 'truncated'/'nextOffset'. "
  "'bounds': true adds each object's world AABB. detail:'summary' returns only {id,name,type}; detail:'xml' returns full entity XML instead (verbose).",
  R"json({"type":"object","properties":{
	"type":{"type":"string","description":"exact type-name filter, e.g. 'Light'"},
	"name":{"type":"string","description":"case-sensitive name substring filter"},
	"region":{"type":"object","description":"world-AABB filter {min:[x,y,z], max:[x,y,z]}","properties":{"min":{"type":"array","items":{"type":"number"}},"max":{"type":"array","items":{"type":"number"}}}},
	"regionMode":{"type":"string","description":"'intersect' (default, bounds overlap), 'inside' (bounds fully contained), or 'center' (origin in box)"},
	"bounds":{"type":"boolean","description":"include world AABB per row"},
	"detail":{"type":"string","description":"'summary' (id/name/type only), 'compact' (default), or 'xml'"},
	"limit":{"type":"integer","description":"max rows returned (default 200)"},
	"offset":{"type":"integer","description":"skip this many matches before returning (for paging; default 0)"}}})json",
  [](cMCPToolCtx& c) {
	std::string sDetail = JStrArg(c.margs, "detail");
	bool bXml     = sDetail=="xml";
	bool bSummary = sDetail=="summary";
	bool bBounds  = JBoolArg(c.margs, "bounds", false);
	const JValue* pTypeFilter = JFind(c.margs, "type");
	std::string sTypeFilter = JStrOf(pTypeFilter);
	std::string sNameFilter = JStrArg(c.margs, "name");
	int lLimit = JIntArg(c.margs, "limit", 200);
	if(lLimit<=0) lLimit = 200;
	int lOffset = JIntArg(c.margs, "offset", 0);
	if(lOffset<0) lOffset = 0;

	// Optional world-AABB region filter (min/max normalized so either draw order works).
	const JValue* pRegion = JFind(c.margs, "region");
	bool bRegion = pRegion && pRegion->IsObject();
	cVector3f vRMin(0), vRMax(0); int lRegionMode = eRegionMode_Intersect;
	if(bRegion)
	{
		cVector3f v0 = JVec3Of(JFind(*pRegion,"min"), cVector3f(-1.0e9f));
		cVector3f v1 = JVec3Of(JFind(*pRegion,"max"), cVector3f( 1.0e9f));
		vRMin = cMath::Vector3Min(v0, v1);
		vRMax = cMath::Vector3Max(v0, v1);
		std::string sMode = JStrArg(c.margs, "regionMode");
		if(sMode=="inside")      lRegionMode = eRegionMode_Inside;
		else if(sMode=="center") lRegionMode = eRegionMode_Center;
	}

	JDoc d; d.SetObject();
	JAlloc& a = d.GetAllocator();
	JValue arr(rapidjson::kArrayType);
	int lTotalMatches = 0;
	tEntityWrapperMap& ents = c.mpWorld->GetEntities();
	for(tEntityWrapperMapIt it=ents.begin(); it!=ents.end(); ++it)
	{
		iEntityWrapper* e = it->second;
		if(e==NULL) continue;
		if(pTypeFilter && cString::To8Char(e->GetTypeName())!=sTypeFilter) continue;
		if(sNameFilter.empty()==false && tString(e->GetName()).find(sNameFilter)==tString::npos) continue;
		if(bRegion && EntityInRegion(e, vRMin, vRMax, lRegionMode)==false) continue;
		int lIdx = lTotalMatches++;             // index among all matches
		if(lIdx < lOffset) continue;            // before the page window
		if((int)arr.Size() >= lLimit) continue; // page full (still counted in totalMatches)
		if(bXml)
		{
			JValue j(rapidjson::kObjectType);
			j.AddMember("id", e->GetID(), a);
			AddEntityXml(j, e, a);
			arr.PushBack(j, a);
		}
		else if(bSummary)
			arr.PushBack(EntitySummary(e, a), a);
		else
			arr.PushBack(EntityCompact(c.mpEditor, e, bBounds, a), a);
	}
	d.AddMember("count", (int)arr.Size(), a);
	d.AddMember("totalMatches", lTotalMatches, a);
	d.AddMember("offset", lOffset, a);
	if(lTotalMatches > lOffset + (int)arr.Size())
	{
		d.AddMember("truncated", true, a);
		d.AddMember("nextOffset", lOffset + (int)arr.Size(), a);
	}
	d.AddMember("entities", arr, a);
	return MakeDoc(d);
  } },

{ "get_entity",
  "Get one object by 'id' or 'name': compact fields + world AABB + full entity XML.",
  R"json({"type":"object","properties":{"id":{"type":"integer"},"name":{"type":"string"}}})json",
  [](cMCPToolCtx& c) {
	iEntityWrapper* e = ResolveEntity(c.mpWorld, c.margs);
	if(e==NULL) return MakeErr("entity not found (pass id or name)");
	JDoc d; d.SetObject();
	JAlloc& a = d.GetAllocator();
	JValue j = EntityCompact(c.mpEditor, e, true, a);
	AddEntityXml(j, e, a);
	// splice j's members into the reply doc root
	for(JValue::MemberIterator it=j.MemberBegin(); it!=j.MemberEnd(); ++it)
		d.AddMember(it->name, it->value, a);
	return MakeDoc(d);
  } },

{ "list_properties",
  "List the typed, settable properties of an object ('id'/'name') or of a type ('type'/'typeId'), "
  "as {name, type, value?} — value only when an instance is given. These are the names create_entity's "
  "'properties' and set_property accept. For a user-defined instance (scripted entity etc.) the reply also "
  "carries a 'variables' array [{name, type, value, default?, description?, enum?}] — the user/script "
  "variables that set_variable and edit_entity's 'variables' write.",
  R"json({"type":"object","properties":{
	"id":{"type":"integer","description":"instance id"},
	"name":{"type":"string","description":"instance name"},
	"type":{"type":"string","description":"type name or xmlName, e.g. 'PointLight'"},
	"typeId":{"type":"integer","description":"type id"}}})json",
  [](cMCPToolCtx& c) {
	iEntityWrapper* e = ResolveEntity(c.mpWorld, c.margs);
	iEntityWrapperType* pType = e ? e->GetType() : ResolveCreateType(c.mpEditor, c.mpWorld, c.margs);
	if(pType==NULL) return MakeErr("no entity or type found (pass id/name or type/typeId)");

	JDoc d; d.SetObject();
	JAlloc& a = d.GetAllocator();
	JValue arr(rapidjson::kArrayType);
	const std::vector<tPropList>& vLists = pType->GetPropLists();
	for(size_t i=0;i<vLists.size();++i)
	{
		for(tPropList::const_iterator it=vLists[i].begin(); it!=vLists[i].end(); ++it)
		{
			iProp* p = *it;
			if(p==NULL) continue;
			JValue row(rapidjson::kObjectType);
			row.AddMember("name", JValue(p->GetName(), a), a);
			row.AddMember("type", rapidjson::StringRef(PropTypeName(p->GetType())), a);
			if(e) AddPropValue(row, "value", e, p, a);
			arr.PushBack(row, a);
		}
	}
	d.AddMember("type", JValue(cString::To8Char(pType->GetName()), a), a);
	d.AddMember("count", (int)arr.Size(), a);
	d.AddMember("properties", arr, a);

	// User/script variables — only present on user-defined instances (scripted
	// entities etc.). These are what set_variable / edit_entity's 'variables' write.
	iEntityWrapperUserDefinedEntity* pUser = e ? AsUserDefined(e) : NULL;
	cEditorClassInstance* pClass = pUser ? pUser->GetClass() : NULL;
	if(pClass)
	{
		JValue varr(rapidjson::kArrayType);
		for(int vi=0; vi<pClass->GetVarInstanceNum(); ++vi)
		{
			cEditorVarInstance* pVarInst = pClass->GetVarInstance(vi);
			if(pVarInst==NULL) continue;
			JValue vrow(rapidjson::kObjectType);
			vrow.AddMember("name",  JValue(cString::To8Char(pVarInst->GetName()),  a), a);
			vrow.AddMember("value", JValue(cString::To8Char(pVarInst->GetValue()), a), a);
			iEditorVar* pVar = pVarInst->GetVarType();
			if(pVar)
			{
				vrow.AddMember("type", rapidjson::StringRef(PropTypeName(pVar->GetType())), a);
				if(pVar->GetDefaultValue().empty()==false)
					vrow.AddMember("default", JValue(cString::To8Char(pVar->GetDefaultValue()), a), a);
				if(pVar->GetDescription().empty()==false)
					vrow.AddMember("description", JValue(cString::To8Char(pVar->GetDescription()), a), a);
				if(pVar->GetType()==eVariableType_Enum)
				{
					const tWStringVec& vVals = static_cast<cEditorVarEnum*>(pVar)->GetEnumValues();
					JValue earr(rapidjson::kArrayType);
					for(size_t ei=0; ei<vVals.size(); ++ei)
						earr.PushBack(JValue(cString::To8Char(vVals[ei]), a), a);
					vrow.AddMember("enum", earr, a);
				}
			}
			varr.PushBack(vrow, a);
		}
		d.AddMember("variables", varr, a);
	}
	return MakeDoc(d);
  } },

{ "get_property",
  "Read a named property of an object.",
  R"json({"type":"object","properties":{
	"id":{"type":"integer"},"name":{"type":"string","description":"entity name"},
	"property":{"type":"string","description":"property name"}},"required":["property"]})json",
  [](cMCPToolCtx& c) {
	iEntityWrapper* e = ResolveEntity(c.mpWorld, c.margs);
	if(e==NULL) return MakeErr("entity not found");
	std::string sProp = JStrArg(c.margs, "property");
	if(sProp.empty()) sProp = JStrArg(c.margs, "name"); // legacy shape: name = property
	if(sProp.empty()) return MakeErr("missing 'property'");
	iProp* pProp = e->GetType()->GetPropByName(sProp);
	if(pProp==NULL) return MakeErr("no such property: " + sProp + " (see list_properties)");
	JDoc d; d.SetObject();
	JAlloc& a = d.GetAllocator();
	d.AddMember("name", JValue(sProp, a), a);
	if(AddPropValue(d, "value", e, pProp, a)==false)
		return MakeErr("unsupported property type");
	return MakeDoc(d);
  } },

{ "get_selection",
  "List the currently selected objects (compact rows).",
  R"json({"type":"object","properties":{}})json",
  [](cMCPToolCtx& c) {
	JDoc d; d.SetObject();
	JAlloc& a = d.GetAllocator();
	JValue arr(rapidjson::kArrayType);
	tEntityWrapperList& lst = c.mpEditor->GetSelection()->GetEntities();
	for(tEntityWrapperListIt it=lst.begin(); it!=lst.end(); ++it)
		if(*it) arr.PushBack(EntityCompact(c.mpEditor, *it, false, a), a);
	d.AddMember("count", (int)arr.Size(), a);
	d.AddMember("entities", arr, a);
	return MakeDoc(d);
  } },

{ "list_entity_types",
  "List creatable entity types, deduplicated: one row per type name with its xmlNames "
  "(pass either to create_entity). 'Entity' subtypes are resolved from the .ent file itself.",
  R"json({"type":"object","properties":{}})json",
  [](cMCPToolCtx& c) {
	// Preserve registration order, merge duplicates by display name.
	std::vector<std::string> vOrder;
	std::map<std::string, std::pair<int, std::vector<std::string>>> mapTypes;
	int n = c.mpWorld->GetEntityTypeNum();
	for(int i=0;i<n;++i)
	{
		iEntityWrapperType* t = c.mpWorld->GetEntityType(i);
		if(t==NULL) continue;
		std::string sName = cString::To8Char(t->GetName());
		if(mapTypes.count(sName)==0) { vOrder.push_back(sName); mapTypes[sName].first = t->GetID(); }
		std::vector<std::string>& vXml = mapTypes[sName].second;
		if(std::find(vXml.begin(), vXml.end(), t->GetXmlElementName())==vXml.end())
			vXml.push_back(t->GetXmlElementName());
	}
	JDoc d; d.SetObject();
	JAlloc& a = d.GetAllocator();
	JValue arr(rapidjson::kArrayType);
	for(size_t i=0;i<vOrder.size();++i)
	{
		std::pair<int, std::vector<std::string>>& info = mapTypes[vOrder[i]];
		JValue e(rapidjson::kObjectType);
		e.AddMember("name", JValue(vOrder[i], a), a);
		e.AddMember("id",   info.first, a);
		JValue xml(rapidjson::kArrayType);
		for(size_t k=0;k<info.second.size();++k) xml.PushBack(JValue(info.second[k], a), a);
		e.AddMember("xmlNames", xml, a);
		arr.PushBack(e, a);
	}
	d.AddMember("count", (int)arr.Size(), a);
	d.AddMember("types", arr, a);
	return MakeDoc(d);
  } },

{ "list_edit_modes",
  "List the editor's edit modes and which one is active.",
  R"json({"type":"object","properties":{}})json",
  [](cMCPToolCtx& c) {
	JDoc d; d.SetObject();
	JAlloc& a = d.GetAllocator();
	JValue arr(rapidjson::kArrayType);
	tEditorEditModeVec& modes = c.mpEditor->GetEditModes();
	iEditorEditMode* pCur = c.mpEditor->GetCurrentEditMode();
	for(size_t i=0;i<modes.size();++i)
	{
		if(modes[i]==NULL) continue;
		JValue e(rapidjson::kObjectType);
		e.AddMember("name",    JValue(modes[i]->GetName(), a), a);
		e.AddMember("current", (modes[i]==pCur), a);
		arr.PushBack(e, a);
	}
	d.AddMember("modes", arr, a);
	return MakeDoc(d);
  } },

{ "get_map_info",
  "Current map filename, modified flag, entity counts (total and per type), skybox and fog settings.",
  R"json({"type":"object","properties":{}})json",
  [](cMCPToolCtx& c) {
	JDoc d; d.SetObject();
	JAlloc& a = d.GetAllocator();
	d.AddMember("filename",    JValue(cString::To8Char(c.mpEditor->GetCurrentMapFilename()), a), a);
	d.AddMember("modified",    c.mpWorld->IsModified(), a);
	d.AddMember("entityCount", (int)c.mpWorld->GetEntities().size(), a);

	std::map<std::string,int> mapCounts;
	tEntityWrapperMap& ents = c.mpWorld->GetEntities();
	for(tEntityWrapperMapIt it=ents.begin(); it!=ents.end(); ++it)
		if(it->second) mapCounts[cString::To8Char(it->second->GetTypeName())]++;
	JValue counts(rapidjson::kObjectType);
	for(std::map<std::string,int>::iterator it=mapCounts.begin(); it!=mapCounts.end(); ++it)
	{
		JValue k(it->first, a);
		counts.AddMember(k, it->second, a);
	}
	d.AddMember("typeCounts", counts, a);

	JValue sky(rapidjson::kObjectType);
	sky.AddMember("show",    c.mpWorld->GetShowSkybox(), a);
	sky.AddMember("active",  c.mpWorld->GetSkyboxActive(), a);
	sky.AddMember("texture", JValue(c.mpWorld->GetSkyboxTexture(), a), a);
	sky.AddMember("color",   JColor(c.mpWorld->GetSkyboxColor(), a), a);
	d.AddMember("skybox", sky, a);

	JValue fog(rapidjson::kObjectType);
	fog.AddMember("show",    c.mpWorld->GetShowFog(), a);
	fog.AddMember("active",  c.mpWorld->GetFogActive(), a);
	fog.AddMember("culling", c.mpWorld->GetFogCulling(), a);
	fog.AddMember("start",   c.mpWorld->GetFogStart(), a);
	fog.AddMember("end",     c.mpWorld->GetFogEnd(), a);
	fog.AddMember("falloff", c.mpWorld->GetFogFalloffExp(), a);
	fog.AddMember("color",   JColor(c.mpWorld->GetFogColor(), a), a);
	d.AddMember("fog", fog, a);

	return MakeDoc(d);
  } },

{ "find_assets",
  "Search the game's indexed resource files (models, .ent entities, particles, materials, sounds...). "
  "'query' is a case-insensitive path substring (e.g. 'castlebase/wall'); 'ext' filters by extension(s) "
  "(e.g. 'ent' or ['dae','msh']). Returns paths usable as create_entity 'file'. "
  "Note: the index is built at editor startup; files created since then are not listed.",
  R"json({"type":"object","properties":{
	"query":{"type":"string","description":"case-insensitive substring of the path"},
	"ext":{"description":"extension string or array of extensions, without dot"},
	"limit":{"type":"integer","description":"max results (default 200)"}}})json",
  [](cMCPToolCtx& c) {
	std::string sQuery = cString::ToLowerCase(JStrArg(c.margs, "query"));
	int lLimit = JIntArg(c.margs, "limit", 200);
	if(lLimit<=0) lLimit = 200;

	std::set<std::string> setExts;
	const JValue* pExt = JFind(c.margs, "ext");
	if(pExt && pExt->IsString())
		setExts.insert(cString::ToLowerCase(JStrOf(pExt)));
	else if(pExt && pExt->IsArray())
		for(JValue::ConstValueIterator it=pExt->Begin(); it!=pExt->End(); ++it)
			if(it->IsString()) setExts.insert(cString::ToLowerCase(JStrOf(&*it)));

	cFileSearcher* pFS = c.mpEditor->GetEngine()->GetResources()->GetFileSearcher();
	const tFilePathMap& mapFiles = pFS->GetAllFiles();

	JDoc d; d.SetObject();
	JAlloc& a = d.GetAllocator();
	int lTotalMatches = 0;
	JValue arr(rapidjson::kArrayType);
	for(tFilePathMap::const_iterator it=mapFiles.begin(); it!=mapFiles.end(); ++it)
	{
		std::string sPath = cString::To8Char(it->second.msPath);
		if(setExts.empty()==false && setExts.count(cString::ToLowerCase(cString::GetFileExt(sPath)))==0) continue;
		if(sQuery.empty()==false && cString::ToLowerCase(sPath).find(sQuery)==std::string::npos) continue;
		++lTotalMatches;
		if(lTotalMatches<=lLimit) arr.PushBack(JValue(sPath, a), a);
	}
	d.AddMember("count", (int)arr.Size(), a);
	d.AddMember("totalMatches", lTotalMatches, a);
	if(lTotalMatches>lLimit) d.AddMember("truncated", true, a);
	d.AddMember("files", arr, a);
	return MakeDoc(d);
  } },

{ "get_asset_bounds",
  "Local-space AABB {min, max, size, center} of a model file (.dae/.msh) or of the mesh referenced by an "
  ".ent file — WITHOUT placing anything. Use it to size/align pieces before create_entity.",
  R"json({"type":"object","properties":{
	"file":{"type":"string","description":"resource-relative model or .ent path"}},"required":["file"]})json",
  [](cMCPToolCtx& c) {
	std::string sFile = JStrArg(c.margs, "file");
	if(sFile.empty()) return MakeErr("missing 'file'");

	// .ent: read the mesh filename out of the entity XML first.
	std::string sMeshFile = sFile;
	if(cString::ToLowerCase(cString::GetFileExt(sFile))=="ent")
	{
		tinyxml2::XMLElement* pRoot = c.mpEditor->GetEngine()->GetResources()->LoadXmlDocument(sFile);
		if(pRoot==NULL) return MakeErr("could not load .ent: " + sFile);
		tinyxml2::XMLElement* pModelData = pRoot->FirstChildElement("ModelData");
		tinyxml2::XMLElement* pMesh = pModelData ? pModelData->FirstChildElement("Mesh") : NULL;
		const char* pMeshFile = pMesh ? pMesh->Attribute("Filename") : NULL;
		sMeshFile = pMeshFile ? pMeshFile : "";
		c.mpEditor->GetEngine()->GetResources()->DestroyXmlDocument(pRoot);
		if(sMeshFile.empty()) return MakeErr("no ModelData/Mesh Filename in " + sFile);
	}

	cMeshManager* pMM = c.mpEditor->GetEngine()->GetResources()->GetMeshManager();
	cMesh* pMesh = pMM->CreateMesh(sMeshFile);
	if(pMesh==NULL) return MakeErr("could not load mesh: " + sMeshFile);

	cVector3f vMin(1e30f), vMax(-1e30f);
	bool bAny = false;
	for(int i=0;i<pMesh->GetSubMeshNum();++i)
	{
		cSubMesh* pSub = pMesh->GetSubMesh(i);
		cVertexBuffer* pVB = pSub ? pSub->GetVertexBuffer() : NULL;
		if(pVB==NULL) continue;
		cBoundingVolume bv = pVB->CreateBoundingVolume();
		vMin = cMath::Vector3Min(vMin, bv.GetMin());
		vMax = cMath::Vector3Max(vMax, bv.GetMax());
		bAny = true;
	}
	pMM->Destroy(pMesh);
	if(bAny==false) return MakeErr("mesh has no vertex data: " + sMeshFile);

	JDoc d; d.SetObject();
	JAlloc& a = d.GetAllocator();
	d.AddMember("file", JValue(sFile, a), a);
	if(sMeshFile!=sFile) d.AddMember("meshFile", JValue(sMeshFile, a), a);
	d.AddMember("bounds", JBounds(vMin, vMax, a), a);
	return MakeDoc(d);
  } },

{ "list_object_library",
  "Browse the editor's placeable object library (the same tree shown in the Static Objects / Entities "
  "browsers). Returns 'categories' (folder rel-paths) and 'objects' [{name, file, relPath, category, type, "
  "triangleCount, bounds}]. 'file' is usable as create_entity / get_asset_bounds / get_mesh_data 'file'. "
  "types: 'all' (default), 'static' (.dae models), 'entity' (.ent). Scope with 'category' (folder substring) "
  "and 'query' (name/path substring) to keep calls cheap; bounds/tri-count come from the browser's on-disk "
  "cache, so a first cold call on a large library scans disk once and can briefly stall the editor.",
  R"json({"type":"object","properties":{
	"types":{"type":"string","description":"'all' (default), 'static', or 'entity'"},
	"category":{"type":"string","description":"case-insensitive folder-path substring filter"},
	"query":{"type":"string","description":"case-insensitive name/path substring filter"},
	"bounds":{"type":"boolean","description":"include cached local AABB per object (default true)"},
	"limit":{"type":"integer","description":"max objects returned (default 500)"}}})json",
  [](cMCPToolCtx& c) {
	const std::string sTypes = JStrArg(c.margs, "types", "all");
	const std::string sCat   = cString::ToLowerCase(JStrArg(c.margs, "category"));
	const std::string sQuery = cString::ToLowerCase(JStrArg(c.margs, "query"));
	const bool bBounds = JBoolArg(c.margs, "bounds", true);
	int lLimit = JIntArg(c.margs, "limit", 500);
	if(lLimit<=0) lLimit = 500;

	const bool bStatic = (sTypes=="all" || sTypes=="static");
	const bool bEntity = (sTypes=="all" || sTypes=="entity");
	if(bStatic==false && bEntity==false)
		return MakeErr("'types' must be 'all', 'static', or 'entity'");

	JDoc d; d.SetObject();
	JAlloc& a = d.GetAllocator();
	JValue arr(rapidjson::kArrayType);
	std::set<std::string> setCats;
	int lTotal = 0;

	if(bStatic)
	{
		tWStringVec vDirs = c.mpEditor->GetLookUpDirs(eDir_StaticObjects);
		for(size_t i=0;i<vDirs.size();++i)
		{
			cEditorObjectIndexStaticObjects* pIndex = hplNew(cEditorObjectIndexStaticObjects, (c.mpEditor, vDirs[i]));
			pIndex->Create();
			MCPCollectIndexEntries(pIndex->GetRootDir(), "static", sCat, sQuery, bBounds, lLimit, arr, a, setCats, lTotal);
			hplDelete(pIndex);
		}
	}
	if(bEntity)
	{
		tWStringVec vDirs = c.mpEditor->GetLookUpDirs(eDir_Entities);
		for(size_t i=0;i<vDirs.size();++i)
		{
			cEditorObjectIndexEntities* pIndex = hplNew(cEditorObjectIndexEntities, (c.mpEditor, vDirs[i]));
			pIndex->Create();
			MCPCollectIndexEntries(pIndex->GetRootDir(), "entity", sCat, sQuery, bBounds, lLimit, arr, a, setCats, lTotal);
			hplDelete(pIndex);
		}
	}

	JValue cats(rapidjson::kArrayType);
	for(std::set<std::string>::iterator it=setCats.begin(); it!=setCats.end(); ++it)
		cats.PushBack(JValue(*it, a), a);

	d.AddMember("count",        (int)arr.Size(), a);
	d.AddMember("totalMatches", lTotal, a);
	if(lTotal > (int)arr.Size()) d.AddMember("truncated", true, a);
	d.AddMember("categories",   cats, a);
	d.AddMember("objects",      arr, a);
	return MakeDoc(d);
  } },

{ "get_mesh_data",
  "Full CPU geometry of a model (.dae/.msh) or the mesh referenced by a .ent, WITHOUT placing anything. "
  "Per submesh: name, material, local transform (16 floats, row-major), vertex/triangle counts, local AABB, "
  "and geometry (positions, normals, uvs, indices). All meter-space. Cap output with 'maxTriangles'; use "
  "'submesh' (index or name) to page a large mesh one submesh at a time. format 'json' (numeric arrays) or "
  "'base64' (little-endian float32 xyz/normal, float32 uv pairs, uint32 indices).",
  R"json({"type":"object","properties":{
	"file":{"type":"string","description":"resource-relative .dae/.msh or .ent path"},
	"submesh":{"description":"optional submesh index (integer) or name (string) to return only that one"},
	"maxTriangles":{"type":"integer","description":"cap total triangles whose geometry is emitted (default 20000); metadata is always listed"},
	"format":{"type":"string","description":"'json' (default) or 'base64'"},
	"includeNormals":{"type":"boolean","description":"include per-vertex normals (default true)"},
	"includeUVs":{"type":"boolean","description":"include per-vertex UVs / texcoord0 (default true)"}},"required":["file"]})json",
  [](cMCPToolCtx& c) {
	std::string sFile = JStrArg(c.margs, "file");
	if(sFile.empty()) return MakeErr("missing 'file'");

	// .ent: read the mesh filename out of the entity XML first (as get_asset_bounds).
	std::string sMeshFile = sFile;
	if(cString::ToLowerCase(cString::GetFileExt(sFile))=="ent")
	{
		tinyxml2::XMLElement* pRoot = c.mpEditor->GetEngine()->GetResources()->LoadXmlDocument(sFile);
		if(pRoot==NULL) return MakeErr("could not load .ent: " + sFile);
		tinyxml2::XMLElement* pModelData = pRoot->FirstChildElement("ModelData");
		tinyxml2::XMLElement* pMesh = pModelData ? pModelData->FirstChildElement("Mesh") : NULL;
		const char* pMeshFile = pMesh ? pMesh->Attribute("Filename") : NULL;
		sMeshFile = pMeshFile ? pMeshFile : "";
		c.mpEditor->GetEngine()->GetResources()->DestroyXmlDocument(pRoot);
		if(sMeshFile.empty()) return MakeErr("no ModelData/Mesh Filename in " + sFile);
	}

	cMeshManager* pMM = c.mpEditor->GetEngine()->GetResources()->GetMeshManager();
	cMesh* pMesh = pMM->CreateMesh(sMeshFile);
	if(pMesh==NULL) return MakeErr("could not load mesh: " + sMeshFile);

	int lMaxTris = JIntArg(c.margs, "maxTriangles", 20000);
	if(lMaxTris<0) lMaxTris = 0;
	const bool bBase64  = (JStrArg(c.margs, "format")=="base64");
	const bool bNormals = JBoolArg(c.margs, "includeNormals", true);
	const bool bUVs     = JBoolArg(c.margs, "includeUVs", true);

	const JValue* pSub = JFind(c.margs, "submesh");
	int         lOnlyIdx = -1;
	std::string sOnlyName;
	if(pSub && pSub->IsInt())         lOnlyIdx  = pSub->GetInt();
	else if(pSub && pSub->IsString()) sOnlyName = JStrOf(pSub);

	JDoc d; d.SetObject();
	JAlloc& a = d.GetAllocator();
	d.AddMember("file", JValue(sFile, a), a);
	if(sMeshFile!=sFile) d.AddMember("meshFile", JValue(sMeshFile, a), a);
	d.AddMember("format", JValue(bBase64 ? "base64" : "json", a), a);

	JValue subs(rapidjson::kArrayType);
	int lEmittedTris = 0;
	bool bTruncated = false;
	cVector3f vWholeMin(1e30f), vWholeMax(-1e30f);
	bool bAny = false;

	for(int i=0;i<pMesh->GetSubMeshNum();++i)
	{
		cSubMesh* pS = pMesh->GetSubMesh(i);
		if(pS==NULL) continue;
		if(lOnlyIdx>=0 && i!=lOnlyIdx) continue;
		if(sOnlyName.empty()==false && tString(pS->GetName())!=sOnlyName) continue;

		JValue sm(rapidjson::kObjectType);
		sm.AddMember("index",    i, a);
		sm.AddMember("name",     JValue(pS->GetName(), a), a);
		sm.AddMember("material", JValue(pS->GetMaterialName(), a), a);
		{
			JValue tr(rapidjson::kArrayType);
			const cMatrixf& mtx = pS->GetLocalTransform();
			for(int k=0;k<16;++k) tr.PushBack(mtx.v[k], a);
			sm.AddMember("transform", tr, a);
		}

		cVertexBuffer* pVB = pS->GetVertexBuffer();
		if(pVB==NULL || (pVB->GetVertexElementFlags() & eVertexElementFlag_Position)==0)
		{
			sm.AddMember("vertexCount",   0, a);
			sm.AddMember("triangleCount", 0, a);
			subs.PushBack(sm, a);
			continue;
		}

		const int lVtx = pVB->GetVertexNum();
		const int lIdx = pVB->GetIndexNum();
		const int lTris = lIdx/3;
		sm.AddMember("vertexCount",   lVtx, a);
		sm.AddMember("triangleCount", lTris, a);

		cBoundingVolume bv = pVB->CreateBoundingVolume();
		sm.AddMember("bounds", JBounds(bv.GetMin(), bv.GetMax(), a), a);
		vWholeMin = cMath::Vector3Min(vWholeMin, bv.GetMin());
		vWholeMax = cMath::Vector3Max(vWholeMax, bv.GetMax());
		bAny = true;

		// Geometry, honouring the triangle cap (metadata above is always emitted).
		if(lMaxTris>0 && lEmittedTris+lTris > lMaxTris)
		{
			bTruncated = true;
			sm.AddMember("geometryOmitted", true, a);
			subs.PushBack(sm, a);
			continue;
		}
		lEmittedTris += lTris;

		const tVertexElementFlag vf = pVB->GetVertexElementFlags();
		const float* pPos = pVB->GetFloatArray(eVertexBufferElement_Position);
		const int    lPosN = pVB->GetElementNum(eVertexBufferElement_Position);
		const float* pNrm  = (bNormals && (vf & eVertexElementFlag_Normal))   ? pVB->GetFloatArray(eVertexBufferElement_Normal)   : NULL;
		const int    lNrmN = pNrm ? pVB->GetElementNum(eVertexBufferElement_Normal) : 0;
		const float* pUV   = (bUVs && (vf & eVertexElementFlag_Texture0))     ? pVB->GetFloatArray(eVertexBufferElement_Texture0) : NULL;
		const int    lUVN  = pUV ? pVB->GetElementNum(eVertexBufferElement_Texture0) : 0;
		const unsigned int* pInd = pVB->GetIndices();

		if(bBase64)
		{
			std::vector<float> vTmp;
			vTmp.reserve((size_t)lVtx*3);
			for(int v=0;v<lVtx;++v) { vTmp.push_back(pPos[v*lPosN+0]); vTmp.push_back(pPos[v*lPosN+1]); vTmp.push_back(pPos[v*lPosN+2]); }
			sm.AddMember("positions", JValue(MCPBase64((const unsigned char*)vTmp.data(), vTmp.size()*sizeof(float)), a), a);
			if(pNrm)
			{
				vTmp.clear();
				for(int v=0;v<lVtx;++v) { vTmp.push_back(pNrm[v*lNrmN+0]); vTmp.push_back(pNrm[v*lNrmN+1]); vTmp.push_back(pNrm[v*lNrmN+2]); }
				sm.AddMember("normals", JValue(MCPBase64((const unsigned char*)vTmp.data(), vTmp.size()*sizeof(float)), a), a);
			}
			if(pUV)
			{
				vTmp.clear();
				for(int v=0;v<lVtx;++v) { vTmp.push_back(pUV[v*lUVN+0]); vTmp.push_back(pUV[v*lUVN+1]); }
				sm.AddMember("uvs", JValue(MCPBase64((const unsigned char*)vTmp.data(), vTmp.size()*sizeof(float)), a), a);
			}
			sm.AddMember("indices", JValue(MCPBase64((const unsigned char*)pInd, (size_t)lIdx*sizeof(unsigned int)), a), a);
			sm.AddMember("encoding", "little-endian: positions/normals float32 xyz, uvs float32 uv, indices uint32", a);
		}
		else
		{
			JValue jp(rapidjson::kArrayType);
			for(int v=0;v<lVtx;++v) { jp.PushBack(pPos[v*lPosN+0], a); jp.PushBack(pPos[v*lPosN+1], a); jp.PushBack(pPos[v*lPosN+2], a); }
			sm.AddMember("positions", jp, a);
			if(pNrm)
			{
				JValue jn(rapidjson::kArrayType);
				for(int v=0;v<lVtx;++v) { jn.PushBack(pNrm[v*lNrmN+0], a); jn.PushBack(pNrm[v*lNrmN+1], a); jn.PushBack(pNrm[v*lNrmN+2], a); }
				sm.AddMember("normals", jn, a);
			}
			if(pUV)
			{
				JValue ju(rapidjson::kArrayType);
				for(int v=0;v<lVtx;++v) { ju.PushBack(pUV[v*lUVN+0], a); ju.PushBack(pUV[v*lUVN+1], a); }
				sm.AddMember("uvs", ju, a);
			}
			JValue ji(rapidjson::kArrayType);
			for(int k=0;k<lIdx;++k) ji.PushBack(pInd[k], a);
			sm.AddMember("indices", ji, a);
		}

		subs.PushBack(sm, a);
	}

	pMM->Destroy(pMesh);

	d.AddMember("submeshCount", (int)subs.Size(), a);
	if(bAny) d.AddMember("bounds", JBounds(vWholeMin, vWholeMax, a), a);
	if(bTruncated) d.AddMember("truncated", true, a);
	d.AddMember("submeshes", subs, a);
	return MakeDoc(d);
  } },


// ----- mutations -----

{ "create_entity",
  "Create one object; ONE undo step even with 'properties'. Lights need only 'type' ('Light'=point; "
  "'SpotLight'/'AreaLight' via xmlName). 'Entity'/'Static Object' need 'file'; 'file' also routes to "
  "Particle System (.ps), Sound (.snt) and Billboard (.mat). 'properties' sets typed properties at creation "
  "(names per list_properties). Undoable.",
  MCP_CREATE_SPEC_SCHEMA,
  [](cMCPToolCtx& c) { return CreateEntitiesImpl(c, false); } },

{ "create_entities",
  "Bulk create: 'entities' is an array of create_entity specs. Everything is validated first (a bad spec "
  "fails the whole call with no changes), then created as ONE undo step. Returns compact rows for the "
  "created objects. Preferred over many create_entity calls for scene building.",
  "{\"type\":\"object\",\"properties\":{"
  "\"entities\":{\"type\":\"array\",\"description\":\"array of create_entity specs\",\"items\":" MCP_CREATE_SPEC_SCHEMA "}"
  "},\"required\":[\"entities\"]}",
  [](cMCPToolCtx& c) { return CreateEntitiesImpl(c, true); } },

{ "delete_entity",
  "Delete objects by id/ids. Undoable.",
  R"json({"type":"object","properties":{"id":{"type":"integer"},"ids":{"type":"array","items":{"type":"integer"}}}})json",
  [](cMCPToolCtx& c) {
	tIntList ids = ParseIds(c.margs);
	if(ids.empty()) return MakeErr("no ids given");
	for(tIntListIt it=ids.begin(); it!=ids.end(); ++it)
		if(c.mpWorld->GetEntity(*it)==NULL) return MakeErr("entity id " + std::to_string(*it) + " not found");
	cEditorEditModeSelect* pSel = (cEditorEditModeSelect*)c.mpEditor->GetEditMode("Select");
	c.mpEditor->AddAction(pSel->CreateDeleteEntitiesAction(ids));
	JDoc d; InitOk(d);
	d.AddMember("deleted", (int)ids.size(), d.GetAllocator());
	return MakeDoc(d);
  } },

{ "clone_entity",
  "Clone objects by id/ids; returns the new clones (they also become the selection). Optional 'offset' "
  "[x,y,z] translates the clones (applied as a second undo step).",
  R"json({"type":"object","properties":{
	"id":{"type":"integer"},"ids":{"type":"array","items":{"type":"integer"}},
	"offset":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3,"description":"translation applied to the clones [x,y,z]"}}})json",
  [](cMCPToolCtx& c) {
	tIntList ids = ParseIds(c.margs);
	if(ids.empty()) return MakeErr("no ids given");
	for(tIntListIt it=ids.begin(); it!=ids.end(); ++it)
		if(c.mpWorld->GetEntity(*it)==NULL) return MakeErr("entity id " + std::to_string(*it) + " not found");

	cEditorEditModeSelect* pSel = (cEditorEditModeSelect*)c.mpEditor->GetEditMode("Select");
	c.mpEditor->AddAction(pSel->CreateCloneEntitiesAction(ids));

	// The clone action replaces the selection with the clones.
	tIntList lstNew = c.mpEditor->GetSelection()->GetEntityIDs();

	if(JHas(c.margs, "offset"))
	{
		cVector3f vOffset = JVec3Of(JFind(c.margs, "offset"), cVector3f(0));
		cEditorActionCompoundAction* pCompound = hplNew(cEditorActionCompoundAction, ("MCP clone offset"));
		for(tIntListIt it=lstNew.begin(); it!=lstNew.end(); ++it)
		{
			iEntityWrapper* e = c.mpWorld->GetEntity(*it);
			if(e) pCompound->AddAction(e->CreateSetPropertyActionVector3f(eObjVec3f_Position, e->GetPosition()+vOffset));
		}
		c.mpEditor->AddAction(pCompound);
	}

	JDoc d; InitOk(d);
	JAlloc& a = d.GetAllocator();
	JValue arr(rapidjson::kArrayType);
	for(tIntListIt it=lstNew.begin(); it!=lstNew.end(); ++it)
	{
		iEntityWrapper* e = c.mpWorld->GetEntity(*it);
		if(e) arr.PushBack(EntityCompact(c.mpEditor, e, false, a), a);
	}
	d.AddMember("count", (int)arr.Size(), a);
	d.AddMember("created", arr, a);
	return MakeDoc(d);
  } },

{ "set_transform",
  "Set absolute position/rotation/scale on one object ('id'/'name') or several ('ids'). "
  "All fields together are ONE undo step. Undoable.",
  R"json({"type":"object","properties":{
	"id":{"type":"integer"},"ids":{"type":"array","items":{"type":"integer"}},"name":{"type":"string"},
	"position":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},
	"rotation":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3,"description":"radians"},
	"scale":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3}}})json",
  [](cMCPToolCtx& c) {
	std::string sErr;
	std::vector<iEntityWrapper*> vEnts = ResolveEntities(c.mpWorld, c.margs, sErr);
	if(vEnts.empty()) return MakeErr(sErr);
	if(!JHas(c.margs,"position") && !JHas(c.margs,"rotation") && !JHas(c.margs,"scale"))
		return MakeErr("provide at least one of position/rotation/scale");

	std::vector<iEditorAction*> vActions;
	for(size_t i=0;i<vEnts.size();++i)
	{
		iEntityWrapper* e = vEnts[i];
		if(JHas(c.margs,"position"))
			vActions.push_back(e->CreateSetPropertyActionVector3f(eObjVec3f_Position, JVec3Of(JFind(c.margs,"position"), e->GetPosition())));
		if(JHas(c.margs,"rotation"))
			vActions.push_back(e->CreateSetPropertyActionVector3f(eObjVec3f_Rotation, JVec3Of(JFind(c.margs,"rotation"), e->GetRotation())));
		if(JHas(c.margs,"scale"))
			vActions.push_back(e->CreateSetPropertyActionVector3f(eObjVec3f_Scale, JVec3Of(JFind(c.margs,"scale"), e->GetScale())));
	}
	if(vActions.size()==1)
		c.mpEditor->AddAction(vActions[0]);
	else
	{
		cEditorActionCompoundAction* pCompound = hplNew(cEditorActionCompoundAction, ("MCP set_transform"));
		for(size_t i=0;i<vActions.size();++i) pCompound->AddAction(vActions[i]);
		c.mpEditor->AddAction(pCompound);
	}
	JDoc d; InitOk(d);
	JAlloc& a = d.GetAllocator();
	JValue arr(rapidjson::kArrayType);
	for(size_t i=0;i<vEnts.size();++i) arr.PushBack(EntityCompact(c.mpEditor, vEnts[i], false, a), a);
	d.AddMember("entities", arr, a);
	return MakeDoc(d);
  } },

{ "set_property",
  "Set typed properties on one object ('id'/'name') or several ('ids'). Either a single 'property' + "
  "'value', or 'properties': {name: value, ...} for many at once. Everything is ONE undo step. "
  "Value shape must match the property type (number/bool/string/[x,y,z]/[r,g,b,a] — see list_properties). Undoable.",
  R"json({"type":"object","properties":{
	"id":{"type":"integer"},"ids":{"type":"array","items":{"type":"integer"}},"name":{"type":"string","description":"entity name"},
	"property":{"type":"string","description":"single property name"},
	"value":{"description":"value for 'property'"},
	"properties":{"type":"object","description":"{name: value, ...} to set several"}}})json",
  [](cMCPToolCtx& c) {
	std::string sErr;
	std::vector<iEntityWrapper*> vEnts = ResolveEntities(c.mpWorld, c.margs, sErr);
	if(vEnts.empty()) return MakeErr(sErr);

	// Collect (name, value) pairs from either shape.
	std::vector<std::pair<std::string, const JValue*>> vProps;
	const JValue* pProps = JFind(c.margs, "properties");
	if(pProps && pProps->IsObject())
		for(JValue::ConstMemberIterator it=pProps->MemberBegin(); it!=pProps->MemberEnd(); ++it)
			vProps.push_back(std::make_pair(std::string(it->name.GetString()), &it->value));
	std::string sSingle = JStrArg(c.margs, "property");
	// Legacy shape: with an 'id' present, "name" used to be the property name.
	if(sSingle.empty() && JHas(c.margs,"value") && JFind(c.margs,"id") && JFind(c.margs,"name"))
		sSingle = JStrArg(c.margs, "name");
	if(sSingle.empty()==false)
	{
		const JValue* pVal = JFind(c.margs, "value");
		if(pVal==NULL) return MakeErr("missing 'value' for 'property'");
		vProps.push_back(std::make_pair(sSingle, pVal));
	}
	if(vProps.empty()) return MakeErr("pass 'property'+'value' or 'properties': {name: value, ...}");

	// Validate all (target, prop) pairs before creating any action.
	std::vector<iEditorAction*> vActions;
	int nUnchanged = 0;
	for(size_t i=0;i<vEnts.size();++i)
	{
		for(size_t k=0;k<vProps.size();++k)
		{
			iProp* pProp = vEnts[i]->GetType()->GetPropByName(vProps[k].first);
			if(pProp==NULL)
			{
				for(size_t x=0;x<vActions.size();++x) hplDelete(vActions[x]);
				return MakeErr("no such property '" + vProps[k].first + "' on entity " + std::to_string(vEnts[i]->GetID()) + " (see list_properties)");
			}
			// Idempotent: skip a set that would not change anything. The undo system
			// rejects a no-op as an "invalid action" (nothing to undo) and logs an
			// ERROR; skipping it here avoids both and treats it as success.
			if(PropEqualsCurrent(vEnts[i], pProp, vProps[k].second)) { ++nUnchanged; continue; }
			iEditorAction* pAction = MakePropSetAction(vEnts[i], pProp, vProps[k].second, sErr);
			if(pAction==NULL)
			{
				if(sErr.empty())
					sErr = "could not set property '" + vProps[k].first + "' on entity " + std::to_string(vEnts[i]->GetID());
				for(size_t x=0;x<vActions.size();++x) hplDelete(vActions[x]);
				return MakeErr(sErr);
			}
			vActions.push_back(pAction);
		}
	}
	if(vActions.size()==1)
		c.mpEditor->AddAction(vActions[0]);
	else if(vActions.size()>1)
	{
		cEditorActionCompoundAction* pCompound = hplNew(cEditorActionCompoundAction, ("MCP set_property"));
		for(size_t i=0;i<vActions.size();++i) pCompound->AddAction(vActions[i]);
		c.mpEditor->AddAction(pCompound);
	}
	JDoc d; InitOk(d);
	JAlloc& a = d.GetAllocator();
	d.AddMember("targets", (int)vEnts.size(), a);
	d.AddMember("propertiesSet", (int)vActions.size(), a);
	d.AddMember("unchanged", nUnchanged, a);
	return MakeDoc(d);
  } },

{ "set_variable",
  "Set per-instance user/script variables (the entity's <UserVariables> — script callbacks, custom vars, "
  "e.g. AffectShadows) on one object ('id'/'name') or several ('ids'). Either a single 'variable' + 'value', "
  "or 'variables': {name: value, ...}. Values are stored as strings — pass a string/number/boolean; for "
  "vector/color variables pass the string exactly as it appears in get_entity's XML / list_properties. Only "
  "user-defined entities (e.g. type 'Entity') have variables. Everything is ONE undo step. Undoable.",
  R"json({"type":"object","properties":{
	"id":{"type":"integer"},"ids":{"type":"array","items":{"type":"integer"}},"name":{"type":"string","description":"entity name"},
	"variable":{"type":"string","description":"single variable name"},
	"value":{"description":"value for 'variable'"},
	"variables":{"type":"object","description":"{name: value, ...} to set several"}}})json",
  [](cMCPToolCtx& c) {
	std::string sErr;
	std::vector<iEntityWrapper*> vEnts = ResolveEntities(c.mpWorld, c.margs, sErr);
	if(vEnts.empty()) return MakeErr(sErr);

	std::vector<std::pair<std::string, const JValue*>> vVars = CollectVarPairs(c.margs);
	if(vVars.empty()) return MakeErr("pass 'variable'+'value' or 'variables': {name: value, ...}");

	std::vector<iEditorAction*> vActions;
	if(BuildVarSetActions(vEnts, vVars, vActions, sErr)==false)
	{
		for(size_t x=0;x<vActions.size();++x) hplDelete(vActions[x]);
		return MakeErr(sErr);
	}
	if(vActions.empty()) return MakeErr("nothing to set");
	if(vActions.size()==1)
		c.mpEditor->AddAction(vActions[0]);
	else
	{
		cEditorActionCompoundAction* pCompound = hplNew(cEditorActionCompoundAction, ("MCP set_variable"));
		for(size_t i=0;i<vActions.size();++i) pCompound->AddAction(vActions[i]);
		c.mpEditor->AddAction(pCompound);
	}
	JDoc d; InitOk(d);
	JAlloc& a = d.GetAllocator();
	d.AddMember("targets", (int)vEnts.size(), a);
	d.AddMember("variablesSet", (int)vVars.size(), a);
	return MakeDoc(d);
  } },

{ "edit_entity",
  "Edit an existing object ('id'/'name', or several with 'ids') in ONE undo step: any of 'position'/"
  "'rotation'/'scale', typed 'properties':{name:value}, and user 'variables':{name:value}. Combines "
  "set_transform + set_property + set_variable atomically. Undoable.",
  R"json({"type":"object","properties":{
	"id":{"type":"integer"},"ids":{"type":"array","items":{"type":"integer"}},"name":{"type":"string"},
	"position":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},
	"rotation":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3,"description":"radians"},
	"scale":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},
	"properties":{"type":"object","description":"typed properties {name: value} (see list_properties)"},
	"variables":{"type":"object","description":"user/script variables {name: value}"}}})json",
  [](cMCPToolCtx& c) {
	std::string sErr;
	std::vector<iEntityWrapper*> vEnts = ResolveEntities(c.mpWorld, c.margs, sErr);
	if(vEnts.empty()) return MakeErr(sErr);

	const bool bPos = JHas(c.margs,"position"), bRot = JHas(c.margs,"rotation"), bScl = JHas(c.margs,"scale");
	std::vector<std::pair<std::string, const JValue*>> vProps;
	const JValue* pProps = JFind(c.margs, "properties");
	if(pProps && pProps->IsObject())
		for(JValue::ConstMemberIterator it=pProps->MemberBegin(); it!=pProps->MemberEnd(); ++it)
			vProps.push_back(std::make_pair(std::string(it->name.GetString()), &it->value));
	std::vector<std::pair<std::string, const JValue*>> vVars = CollectVarPairs(c.margs);

	if(!bPos && !bRot && !bScl && vProps.empty() && vVars.empty())
		return MakeErr("nothing to edit (pass position/rotation/scale, properties or variables)");

	// Build every action first; if anything is invalid, free them all and bail
	// (nothing is applied) — the whole edit is atomic.
	std::vector<iEditorAction*> vActions;
	for(size_t i=0;i<vEnts.size();++i)
	{
		iEntityWrapper* e = vEnts[i];
		if(bPos) vActions.push_back(e->CreateSetPropertyActionVector3f(eObjVec3f_Position, JVec3Of(JFind(c.margs,"position"), e->GetPosition())));
		if(bRot) vActions.push_back(e->CreateSetPropertyActionVector3f(eObjVec3f_Rotation, JVec3Of(JFind(c.margs,"rotation"), e->GetRotation())));
		if(bScl) vActions.push_back(e->CreateSetPropertyActionVector3f(eObjVec3f_Scale,    JVec3Of(JFind(c.margs,"scale"),    e->GetScale())));
	}
	for(size_t i=0;i<vEnts.size() && sErr.empty();++i)
		for(size_t k=0;k<vProps.size();++k)
		{
			iProp* pProp = vEnts[i]->GetType()->GetPropByName(vProps[k].first);
			if(pProp==NULL) { sErr = "no such property '" + vProps[k].first + "' on entity " + std::to_string(vEnts[i]->GetID()) + " (see list_properties)"; break; }
			iEditorAction* pAction = MakePropSetAction(vEnts[i], pProp, vProps[k].second, sErr);
			if(pAction==NULL) break;
			vActions.push_back(pAction);
		}
	if(sErr.empty() && vVars.empty()==false)
		BuildVarSetActions(vEnts, vVars, vActions, sErr);

	if(sErr.empty()==false)
	{
		for(size_t x=0;x<vActions.size();++x) hplDelete(vActions[x]);
		return MakeErr(sErr);
	}
	if(vActions.empty()) return MakeErr("nothing to edit");
	if(vActions.size()==1)
		c.mpEditor->AddAction(vActions[0]);
	else
	{
		cEditorActionCompoundAction* pCompound = hplNew(cEditorActionCompoundAction, ("MCP edit_entity"));
		for(size_t i=0;i<vActions.size();++i) pCompound->AddAction(vActions[i]);
		c.mpEditor->AddAction(pCompound);
	}
	JDoc d; InitOk(d);
	JAlloc& a = d.GetAllocator();
	JValue arr(rapidjson::kArrayType);
	for(size_t i=0;i<vEnts.size();++i) arr.PushBack(EntityCompact(c.mpEditor, vEnts[i], false, a), a);
	d.AddMember("entities", arr, a);
	return MakeDoc(d);
  } },

{ "set_entity",
  "Apply an edited version of get_entity's 'xml' back onto an existing object ('id'/'name'). Pass the "
  "entity's XML element string (what get_entity returns in 'xml', with your edits) as 'xml'. The root "
  "element must be the entity's own type (e.g. '<PointLight ...>') — type/subtype cannot change, and any "
  "'id' in the XML is ignored so the object keeps its identity, group and references. Rebuilds the object "
  "from the XML in place (all typed properties + user variables). Undoable.",
  R"json({"type":"object","properties":{
	"id":{"type":"integer"},"name":{"type":"string","description":"entity name"},
	"xml":{"type":"string","description":"edited entity XML element, from get_entity's 'xml' field"}},"required":["xml"]})json",
  [](cMCPToolCtx& c) {
	iEntityWrapper* e = ResolveEntity(c.mpWorld, c.margs);
	if(e==NULL) return MakeErr("entity not found (pass id or name)");
	std::string sXml = JStrArg(c.margs, "xml");
	if(sXml.empty()) return MakeErr("missing 'xml' (pass get_entity's 'xml' string, edited)");

	tinyxml2::XMLDocument doc;
	if(doc.Parse(sXml.c_str())!=tinyxml2::XML_SUCCESS || doc.RootElement()==NULL)
		return MakeErr(std::string("could not parse 'xml': ") + (doc.ErrorStr() ? doc.ErrorStr() : "invalid XML"));

	tinyxml2::XMLElement* pRoot = doc.RootElement();
	iEntityWrapperType* pType = e->GetType();
	const tString& sExpect = pType->GetXmlElementName();
	if(sExpect != pRoot->Name())
		return MakeErr("xml root <" + tString(pRoot->Name()) + "> does not match entity type <" + sExpect +
		               "> (type/subtype cannot change)");

	iEntityWrapperData* pData = pType->CreateData();
	if(pData==NULL) return MakeErr("could not create data for type '" + sExpect + "'");
	pData->Load(pRoot);
	pData->SetID(e->GetID());   // keep the placed entity's identity; ignore any id in the XML

	c.mpEditor->AddAction(hplNew(cEditorActionEntitySetData, (c.mpWorld, e->GetID(), pData)));

	JDoc d; InitOk(d);
	JAlloc& a = d.GetAllocator();
	iEntityWrapper* pAfter = c.mpWorld->GetEntity(e->GetID());
	if(pAfter) d.AddMember("entity", EntityCompact(c.mpEditor, pAfter, false, a), a);
	return MakeDoc(d);
  } },

{ "set_group",
  "Assign objects ('ids') to a group id from list_groups (0 = no group). Undoable.",
  R"json({"type":"object","properties":{
	"ids":{"type":"array","items":{"type":"integer"}},"id":{"type":"integer"},
	"group":{"type":"integer","description":"target group id; 0 clears"}},"required":["group"]})json",
  [](cMCPToolCtx& c) {
	tIntList ids = ParseIds(c.margs);
	if(ids.empty()) return MakeErr("no ids given");
	unsigned int lGroup = (unsigned int)JIntArg(c.margs, "group", 0);
	if(lGroup!=0 && c.mpEditor->GetGroups().count(lGroup)==0)
		return MakeErr("no such group: " + std::to_string(lGroup) + " (see list_groups)");
	for(tIntListIt it=ids.begin(); it!=ids.end(); ++it)
		if(c.mpWorld->GetEntity(*it)==NULL) return MakeErr("entity id " + std::to_string(*it) + " not found");
	c.mpEditor->AddAction(hplNew(cLevelEditorActionGroupSetEntities, (c.mpEditor, lGroup, ids)));
	JDoc d; InitOk(d);
	JAlloc& a = d.GetAllocator();
	d.AddMember("group", lGroup, a);
	d.AddMember("entities", (int)ids.size(), a);
	return MakeDoc(d);
  } },

{ "select",
  "Select objects. mode: 'add' (default), 'set' (replace), 'toggle'.",
  R"json({"type":"object","properties":{
	"ids":{"type":"array","items":{"type":"integer"}},"id":{"type":"integer"},
	"mode":{"type":"string","enum":["add","set","toggle"]}}})json",
  [](cMCPToolCtx& c) { return SelectImpl(c, "select"); } },

{ "deselect",
  "Remove objects from the selection.",
  R"json({"type":"object","properties":{"ids":{"type":"array","items":{"type":"integer"}},"id":{"type":"integer"}}})json",
  [](cMCPToolCtx& c) { return SelectImpl(c, "deselect"); } },

{ "clear_selection",
  "Clear the selection.",
  R"json({"type":"object","properties":{}})json",
  [](cMCPToolCtx& c) { return SelectImpl(c, "clear"); } },

{ "set_edit_mode",
  "Switch the active edit mode by name (see list_edit_modes).",
  R"json({"type":"object","properties":{"name":{"type":"string"}},"required":["name"]})json",
  [](cMCPToolCtx& c) {
	std::string sName = JStrArg(c.margs, "name");
	iEditorEditMode* m = c.mpEditor->GetEditMode(sName);
	if(m==NULL) return MakeErr("no such edit mode: " + sName + " (see list_edit_modes)");
	c.mpEditor->SetCurrentEditMode(m);
	return MakeOk();
  } },

{ "undo",
  "Undo the last action.",
  R"json({"type":"object","properties":{}})json",
  [](cMCPToolCtx& c) { c.mpEditor->GetActionHandler()->Undo(); return MakeOk(); } },

{ "redo",
  "Redo the last undone action.",
  R"json({"type":"object","properties":{}})json",
  [](cMCPToolCtx& c) { c.mpEditor->GetActionHandler()->Redo(); return MakeOk(); } },

// ----- map files -----

{ "new_map",
  "Reset to an empty map (discards unsaved changes).",
  R"json({"type":"object","properties":{}})json",
  [](cMCPToolCtx& c) { c.mpEditor->Reset(); return MakeOk(); } },

{ "save_map",
  "Save the current map to 'path' (.map). Plain filesystem path (absolute or CWD-relative).",
  R"json({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})json",
  [](cMCPToolCtx& c) {
	std::string sPath = JStrArg(c.margs, "path");
	if(sPath.empty()) return MakeErr("missing 'path'");
	if(c.mpEditor->SaveToFile(cString::To16Char(sPath))==false)
		return MakeErr("save failed");
	JDoc d; InitOk(d);
	d.AddMember("path", JValue(sPath, d.GetAllocator()), d.GetAllocator());
	return MakeDoc(d);
  } },

{ "load_map",
  "Load a map from 'path' (.map). Plain filesystem path (absolute or CWD-relative).",
  R"json({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})json",
  [](cMCPToolCtx& c) {
	std::string sPath = JStrArg(c.margs, "path");
	if(sPath.empty()) return MakeErr("missing 'path'");
	if(c.mpEditor->LoadFromFile(cString::To16Char(sPath))==false)
		return MakeErr("load failed (file missing?)");
	JDoc d; InitOk(d);
	JAlloc& a = d.GetAllocator();
	d.AddMember("path", JValue(sPath, a), a);
	d.AddMember("entityCount", (int)c.mpEditor->GetEditorWorld()->GetEntities().size(), a);
	return MakeDoc(d);
  } },

{ "import_map",
  "Import all objects from a .map or .expobj file into the current map. 'path' is tried as a plain "
  "filesystem path first (absolute or CWD-relative — works for freshly generated files), then as a "
  "startup-indexed resource name. NOT undoable.",
  R"json({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})json",
  [](cMCPToolCtx& c) {
	std::string sPath = JStrArg(c.margs, "path");
	if(sPath.empty()) return MakeErr("missing 'path'");

	tIntList imported;
	tinyxml2::XMLDocument doc;
	if(doc.LoadFile(sPath.c_str())==tinyxml2::XML_SUCCESS && doc.RootElement()!=NULL)
	{
		if(c.mpWorld->ImportObjects(doc.RootElement(), imported)==false)
			return MakeErr("'" + sPath + "' has no MapData/MapContents section");
	}
	else
	{
		// Fall back to the resource index (only knows files present at editor startup).
		tinyxml2::XMLElement* pRoot = c.mpEditor->GetEngine()->GetResources()->LoadXmlDocument(sPath);
		if(pRoot==NULL)
			return MakeErr("could not load '" + sPath + "': not a readable file path, and not in the resource index (which only lists files present at editor startup) — pass an absolute path");
		bool bOk = c.mpWorld->ImportObjects(pRoot, imported);
		c.mpEditor->GetEngine()->GetResources()->DestroyXmlDocument(pRoot);
		if(bOk==false)
			return MakeErr("'" + sPath + "' has no MapData/MapContents section");
	}

	JDoc d; InitOk(d);
	JAlloc& a = d.GetAllocator();
	JValue ids(rapidjson::kArrayType);
	for(tIntListIt it=imported.begin(); it!=imported.end(); ++it) ids.PushBack(*it, a);
	d.AddMember("importedCount", (int)imported.size(), a);
	d.AddMember("importedIds", ids, a);
	return MakeDoc(d);
  } },

{ "export_selection",
  "Export the current selection to an .expobj file at 'path' (re-importable via import_map).",
  R"json({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})json",
  [](cMCPToolCtx& c) {
	std::string sPath = JStrArg(c.margs, "path");
	if(sPath.empty()) return MakeErr("missing 'path'");
	cEditorSelection* pSel = c.mpEditor->GetSelection();
	if(pSel->IsEmpty()) return MakeErr("selection is empty");
	tEntityWrapperList lst = pSel->GetEntities();
	c.mpWorld->ExportObjects(sPath, lst);
	JDoc d; InitOk(d);
	d.AddMember("exportedCount", (int)lst.size(), d.GetAllocator());
	return MakeDoc(d);
  } },

// ----- skybox / fog -----

{ "set_skybox",
  "Set skybox properties. Undoable (except 'show').",
  R"json({"type":"object","properties":{
	"show":{"type":"boolean"},"active":{"type":"boolean"},"texture":{"type":"string"},
	"color":{"type":"array","items":{"type":"number"},"description":"[r,g,b,a]"}}})json",
  [](cMCPToolCtx& c) {
	if(JHas(c.margs,"show"))    c.mpWorld->SetShowSkybox(JBoolArg(c.margs,"show"));
	if(JHas(c.margs,"active"))  c.mpEditor->AddAction(hplNew(cLevelEditorActionSetSkyboxActive, (c.mpWorld, JBoolArg(c.margs,"active"))));
	if(JHas(c.margs,"texture")) c.mpEditor->AddAction(hplNew(cLevelEditorActionSetSkyboxTexture, (c.mpWorld, JStrArg(c.margs,"texture"))));
	if(JHas(c.margs,"color"))   c.mpEditor->AddAction(hplNew(cLevelEditorActionSetSkyboxColor, (c.mpWorld, JColorOf(JFind(c.margs,"color"), cColor(1)))));
	return MakeOk();
  } },

{ "set_fog",
  "Set fog properties. Undoable (except 'show').",
  R"json({"type":"object","properties":{
	"show":{"type":"boolean"},"active":{"type":"boolean"},"culling":{"type":"boolean"},
	"start":{"type":"number"},"end":{"type":"number"},"falloff":{"type":"number"},
	"color":{"type":"array","items":{"type":"number"},"description":"[r,g,b,a]"}}})json",
  [](cMCPToolCtx& c) {
	if(JHas(c.margs,"show"))    c.mpWorld->SetShowFog(JBoolArg(c.margs,"show"));
	if(JHas(c.margs,"active"))  c.mpEditor->AddAction(hplNew(cLevelEditorActionSetFogBoolProperty, (c.mpWorld, eFogBoolProp_Active, JBoolArg(c.margs,"active"))));
	if(JHas(c.margs,"culling")) c.mpEditor->AddAction(hplNew(cLevelEditorActionSetFogBoolProperty, (c.mpWorld, eFogBoolProp_Culling, JBoolArg(c.margs,"culling"))));
	if(JHas(c.margs,"start"))   c.mpEditor->AddAction(hplNew(cLevelEditorActionSetFogFloatProperty, (c.mpWorld, eFogFloatProp_Start, (float)JNumArg(c.margs,"start"))));
	if(JHas(c.margs,"end"))     c.mpEditor->AddAction(hplNew(cLevelEditorActionSetFogFloatProperty, (c.mpWorld, eFogFloatProp_End, (float)JNumArg(c.margs,"end"))));
	if(JHas(c.margs,"falloff")) c.mpEditor->AddAction(hplNew(cLevelEditorActionSetFogFloatProperty, (c.mpWorld, eFogFloatProp_FalloffExp, (float)JNumArg(c.margs,"falloff"))));
	if(JHas(c.margs,"color"))   c.mpEditor->AddAction(hplNew(cLevelEditorActionSetFogColor, (c.mpWorld, JColorOf(JFind(c.margs,"color"), cColor(1)))));
	return MakeOk();
  } },

// ----- groups -----

{ "list_groups",
  "List entity groups.",
  R"json({"type":"object","properties":{}})json",
  [](cMCPToolCtx& c) {
	JDoc d; d.SetObject();
	JAlloc& a = d.GetAllocator();
	JValue arr(rapidjson::kArrayType);
	tGroupMap& groups = c.mpEditor->GetGroups();
	for(tGroupMapIt it=groups.begin(); it!=groups.end(); ++it)
	{
		JValue g(rapidjson::kObjectType);
		g.AddMember("id",      it->first, a);
		g.AddMember("name",    JValue(it->second.GetName(), a), a);
		g.AddMember("visible", it->second.GetVisibility(), a);
		arr.PushBack(g, a);
	}
	d.AddMember("groups", arr, a);
	return MakeDoc(d);
  } },

{ "create_group",
  "Create a group. Returns its id (assign members via set_group).",
  R"json({"type":"object","properties":{"name":{"type":"string"}}})json",
  [](cMCPToolCtx& c) {
	std::string sName = JStrArg(c.margs, "name", "Group");
	unsigned int lNewID = 1;
	tGroupMap& groups = c.mpEditor->GetGroups();
	for(tGroupMapIt it=groups.begin(); it!=groups.end(); ++it)
		if(it->first + 1 > lNewID) lNewID = it->first + 1;
	c.mpEditor->AddAction(hplNew(cLevelEditorActionGroupAdd, (c.mpEditor, lNewID, sName)));
	JDoc d; InitOk(d);
	d.AddMember("id", lNewID, d.GetAllocator());
	return MakeDoc(d);
  } },

{ "rename_group",
  "Rename a group by id.",
  R"json({"type":"object","properties":{"id":{"type":"integer"},"name":{"type":"string"}},"required":["id","name"]})json",
  [](cMCPToolCtx& c) {
	if(JHas(c.margs,"id")==false) return MakeErr("missing 'id'");
	c.mpEditor->AddAction(hplNew(cLevelEditorActionGroupSetName, (c.mpEditor, (unsigned int)JIntArg(c.margs,"id"), JStrArg(c.margs,"name"))));
	return MakeOk();
  } },

{ "delete_group",
  "Delete a group by id.",
  R"json({"type":"object","properties":{"id":{"type":"integer"}},"required":["id"]})json",
  [](cMCPToolCtx& c) {
	if(JHas(c.margs,"id")==false) return MakeErr("missing 'id'");
	c.mpEditor->AddAction(hplNew(cLevelEditorActionGroupDelete, (c.mpEditor, (unsigned int)JIntArg(c.margs,"id"))));
	return MakeOk();
  } },

// ----- view / render -----

{ "capture_view",
  "Render the current map from a virtual camera and return a PNG image of the lit scene, so you can "
  "actually SEE what you are editing (not just read entity XML). Either give 'position' (eye) + 'target' "
  "(look-at) as [x,y,z] world coordinates, or 'focus' (an entity id or name) to auto-frame that object "
  "(optional 'distance' overrides how far back the camera sits). Optional 'fov' (vertical, degrees), "
  "'width'/'height' (pixels), 'near'/'far' clip planes default to the focused editor viewport's camera. "
  "Set 'include_visible' to also get back a {\"visible\":[ids]} text block listing the entities in frame. "
  "The render + GPU readback take a few frames.",
  R"json({"type":"object","properties":{
	"position":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3,"description":"eye / camera world position [x,y,z]"},
	"target":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3,"description":"look-at point in world space [x,y,z]"},
	"focus":{"description":"entity id (int) or name (string) to auto-frame instead of position/target"},
	"distance":{"type":"number","description":"camera distance when using 'focus' (default: 2.5 x bounding radius)"},
	"fov":{"type":"number","description":"vertical field of view in degrees (default: focused viewport)"},
	"width":{"type":"integer","description":"image width in pixels (default 1024, clamped to 16..2048)"},
	"height":{"type":"integer","description":"image height in pixels (default 576, clamped to 16..2048)"},
	"near":{"type":"number","description":"near clip plane distance (default: focused viewport)"},
	"far":{"type":"number","description":"far clip plane distance (default: focused viewport)"},
	"include_visible":{"type":"boolean","description":"also return the ids of entities within the captured view (default false)"}}})json",
  [](cMCPToolCtx& c) {
	cLevelEditorCameraCapture* pCap = c.mpEditor->GetCameraCapture();
	if(pCap==NULL || pCap->IsAvailable()==false)
		return MakeErr("camera capture unavailable (headless render setup failed)");

	cVector3f vPos(0), vTarget(0);
	const JValue* pFocus = JFind(c.margs, "focus");
	if(pFocus)
	{
		iEntityWrapper* e = NULL;
		if(pFocus->IsInt())         e = c.mpWorld->GetEntity(pFocus->GetInt());
		else if(pFocus->IsString()) e = c.mpWorld->GetEntityByName(JStrOf(pFocus));
		if(e==NULL) return MakeErr("focus entity not found");

		cBoundingVolume* pBV = e->GetRenderBV();
		cVector3f vCenter = pBV ? pBV->GetWorldCenter() : e->GetPosition();
		float fRadius     = pBV ? cMath::Max(pBV->GetRadius(), 0.5f) : 1.5f;
		float fDist       = JHas(c.margs,"distance") ? (float)JNumArg(c.margs,"distance") : fRadius*2.5f;
		cVector3f vDir    = cMath::Vector3Normalize(cVector3f(1.0f, 0.7f, -1.0f));
		vTarget = vCenter;
		vPos    = vCenter + vDir*fDist;
	}
	else
	{
		if(JHas(c.margs,"position")==false || JHas(c.margs,"target")==false)
			return MakeErr("capture_view needs 'position'+'target' [x,y,z], or 'focus' (entity id/name)");
		vPos    = JVec3Of(JFind(c.margs,"position"), cVector3f(0));
		vTarget = JVec3Of(JFind(c.margs,"target"),   cVector3f(0));
	}
	if(vPos == vTarget)
		return MakeErr("'position' and 'target' must differ");

	// Defaults for omitted camera fields come from the currently-focused
	// editor viewport's camera, so an under-specified capture matches what
	// the user is looking at. 'fov' is given in DEGREES (agent-friendly) and
	// converted to the radians the engine camera expects.
	cEditorWindowViewport* pFocused = c.mpEditor->GetFocusedViewport();
	cCamera* pDefCam = pFocused ? pFocused->GetCamera() : NULL;

	float fFov  = pDefCam ? pDefCam->GetFOV()           : cMath::ToRad(60.0f);
	float fNear = pDefCam ? pDefCam->GetNearClipPlane() : 0.05f;
	float fFar  = pDefCam ? pDefCam->GetFarClipPlane()  : 1000.0f;
	if(JHas(c.margs,"fov"))  fFov  = cMath::ToRad((float)JNumArg(c.margs,"fov"));
	if(JHas(c.margs,"near")) fNear = (float)JNumArg(c.margs,"near");
	if(JHas(c.margs,"far"))  fFar  = (float)JNumArg(c.margs,"far");

	int lWidth  = JIntArg(c.margs, "width",  1024);
	int lHeight = JIntArg(c.margs, "height", 576);
	bool bIncludeVisible = JBoolArg(c.margs, "include_visible", false);

	int lJobId = pCap->Enqueue(vPos, vTarget, fFov, fNear, fFar, lWidth, lHeight, bIncludeVisible);
	if(lJobId < 0)
		return MakeErr("camera capture unavailable");

	// Deferred: the PNG is rendered + read back over the next few frames.
	// DrainQueue parks this caller's promise; the capture pump fulfils it.
	cMCPToolResult r;
	r.mbDeferred   = true;
	r.mlDeferJobId = lJobId;
	return r;
  } },

}; // gvTools

static const int glNumTools = (int)(sizeof(gvTools)/sizeof(gvTools[0]));

//--------------------------------------------------------------------

cLevelEditorMCPCommands::cLevelEditorMCPCommands(cLevelEditor* apEditor)
	: mpEditor(apEditor)
{
}

//--------------------------------------------------------------------
// Dispatch
//--------------------------------------------------------------------

cMCPToolResult cLevelEditorMCPCommands::HandleToolCall(const std::string& asTool, const std::string& asArgsJson)
{
	rapidjson::Document argsDoc;
	argsDoc.Parse(asArgsJson.c_str(), asArgsJson.size());
	if(argsDoc.HasParseError() || argsDoc.IsObject()==false)
		argsDoc.SetObject();

	cLevelEditor* pEditor = mpEditor;
	iEditorWorld* pWorld  = pEditor->GetEditorWorld();
	if(pWorld==NULL) return MakeErr("no editor world");

	for(int i=0;i<glNumTools;++i)
	{
		if(asTool==gvTools[i].msName)
		{
			cMCPToolCtx ctx = { pEditor, pWorld, argsDoc };
			return gvTools[i].mpfHandler(ctx);
		}
	}
	return MakeErr("unknown tool: " + asTool);
}

//--------------------------------------------------------------------
// Tool schemas (tools/list). Generated from the same table as dispatch:
// name/description as strings, each schema literal spliced in raw.
//--------------------------------------------------------------------

std::string cLevelEditorMCPCommands::GetToolSchemasJson()
{
	rapidjson::StringBuffer buf;
	rapidjson::Writer<rapidjson::StringBuffer> w(buf);
	w.StartArray();
	for(int i=0;i<glNumTools;++i)
	{
		w.StartObject();
		w.Key("name");        w.String(gvTools[i].msName);
		w.Key("description"); w.String(gvTools[i].msDesc);
		w.Key("inputSchema"); w.RawValue(gvTools[i].msSchemaJson, strlen(gvTools[i].msSchemaJson), rapidjson::kObjectType);
		w.EndObject();
	}
	w.EndArray();
	return std::string(buf.GetString(), buf.GetSize());
}
