// SciTE - Scintilla based Text Editor
// LuaExtension.cxx - Lua scripting extension
// Copyright 1998-2000 by Neil Hodgson <neilh@scintilla.org>
// The License.txt file describes the conditions under which this software may be distributed.

#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <ctime>

#include <compare>
#include <tuple>
#include <string>
#include <string_view>
#include <vector>
#include <set>
#include <memory>
#include <chrono>

#include "ScintillaTypes.h"
#include "ScintillaMessages.h"
#include "ScintillaCall.h"

#include "GUI.h"
#include "StringHelpers.h"
#include "FilePath.h"
#include "StyleWriter.h"
#include "Extender.h"

#include "IFaceTable.h"
#include "SciTEKeys.h"
#include "LuaExtension.h"

#define LUA_COMPAT_5_1
extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
}

#if (LUA_VERSION_NUM < 502)
#define lua_pushglobaltable(L) lua_pushvalue(L, LUA_GLOBALSINDEX)
#undef  lua_getglobal
#define lua_getglobal(L,s) (lua_getfield(L, LUA_GLOBALSINDEX, s), lua_type(L, -1))
#else
#define LUA_GLOBALSINDEX LUA_RIDX_GLOBALS
#endif

#if defined(_WIN32) && defined(_MSC_VER)

// MSVC looks deeper into the code than other compilers, sees that
// lua_error calls longjmp, and complains about unreachable code.
#pragma warning(disable: 4702)

#endif

namespace SA = Scintilla;

// A note on naming conventions:
// I've gone back and forth on this a bit, trying different styles.
// It isn't easy to get something that feels consistent, considering
// that the Lua API uses lower case, underscore-separated words and
// Scintilla of course uses mixed case with no underscores.

// What I've settled on is that functions that require you to think
// about the Lua stack are likely to be mixed with Lua API functions,
// so these should using a naming convention similar to Lua itself.
// Functions that don't manipulate Lua at a low level should follow
// the normal SciTE convention.  There is some grey area of course,
// and for these I just make a judgement call

namespace {

ExtensionAPI *host = nullptr;
lua_State *luaState = nullptr;
bool luaDisabled = false;

std::string startupScript;
std::string extensionScript;

bool tracebackEnabled = true;

int maxBufferIndex = -1;
int curBufferIndex = -1;

int GetPropertyInt(const char *propName) {
	int propVal = 0;
	if (host) {
		const std::string sPropVal = host->Property(propName);
		propVal = IntegerFromString(sPropVal, 0);
	}
	return propVal;
}

}

LuaExtension::LuaExtension() noexcept = default;

LuaExtension::~LuaExtension() = default;

LuaExtension &LuaExtension::Instance() noexcept {
	static LuaExtension singleton;
	return singleton;
}

namespace {

// Forward declarations
ExtensionAPI::Pane check_pane_object(lua_State *L, int index);
void push_pane_object(lua_State *L, ExtensionAPI::Pane p) noexcept;
int iface_function_helper(lua_State *L, const IFaceFunction &func);

bool IFaceTypeIsScriptable(IFaceType t, int index) noexcept {
	return t < iface_stringresult || (index==1 && t == iface_stringresult);
}

bool IFaceTypeIsNumeric(IFaceType t) noexcept {
	return (t > iface_void && t < iface_bool);
}

bool IFaceFunctionIsScriptable(const IFaceFunction &f) noexcept {
	return IFaceTypeIsScriptable(f.paramType[0], 0) && IFaceTypeIsScriptable(f.paramType[1], 1);
}

bool IFacePropertyIsScriptable(const IFaceProperty &p) noexcept {
	return (((p.valueType > iface_void) && (p.valueType <= iface_stringresult) && (p.valueType != iface_keymod)) &&
		((p.paramType < iface_colour) || (p.paramType == iface_string) ||
		 (p.paramType == iface_bool)) && (p.getter || p.setter));
}

const char *push_string(lua_State *L, const std::string &s) noexcept {
	return lua_pushlstring(L, s.data(), s.length());
}

void raise_error(lua_State *L, const char *errMsg=nullptr) noexcept {
	luaL_where(L, 1);
	if (errMsg) {
		lua_pushstring(L, errMsg);
	} else {
		lua_insert(L, -2);
	}
	lua_concat(L, 2);
	lua_error(L);
}

// lua_absindex for LUA <5.1
int absolute_index(lua_State *L, int index) noexcept {
	if (index > LUA_REGISTRYINDEX && index < 0)
		return lua_gettop(L) + index + 1;
	else
		return index;
}

/**
* merge_table / clone_table / clear_table utilized to
* "soft-replace" an existing global scope instead of using new_table,
* because then startup script would be bound to a different copy
* of the globals than the extension script.
**/

// copy the contents of one table into another returning the size
int merge_table(lua_State *L, int destTableIdx, int srcTableIdx, bool copyMetatable = false) {
	int count = 0;
	if (lua_istable(L, destTableIdx) && lua_istable(L, srcTableIdx)) {
		srcTableIdx = absolute_index(L, srcTableIdx);
		destTableIdx = absolute_index(L, destTableIdx);
		if (copyMetatable) {
			lua_getmetatable(L, srcTableIdx);
			lua_setmetatable(L, destTableIdx);
		}
		lua_pushnil(L); // first key
		while (lua_next(L, srcTableIdx) != 0) {
			// key is at index -2 and value at index -1
			lua_pushvalue(L, -2); // the key
			lua_insert(L, -2); // leaving value (-1), key (-2), key (-3)
			lua_rawset(L, destTableIdx);
			++count;
		}
	}
	return count;
}

// make a copy of a table (not deep), leaving it at the top of the stack
bool clone_table(lua_State *L, int srcTableIdx, bool copyMetatable = false) {
	if (lua_istable(L, srcTableIdx)) {
		srcTableIdx = absolute_index(L, srcTableIdx);
		lua_newtable(L);
		merge_table(L, -1, srcTableIdx, copyMetatable);
		return true;
	} else {
		lua_pushnil(L);
		return false;
	}
}

// loop through each key in the table and set its value to nil
void clear_table(lua_State *L, int tableIdx, bool clearMetatable = true) {
	if (lua_istable(L, tableIdx)) {
		tableIdx = absolute_index(L, tableIdx);
		if (clearMetatable) {
			lua_pushnil(L);
			lua_setmetatable(L, tableIdx);
		}
		lua_pushnil(L); // first key
		while (lua_next(L, tableIdx) != 0) {
			// key is at index -2 and value at index -1
			lua_pop(L, 1); // discard value
			lua_pushnil(L);
			lua_rawset(L, tableIdx); // table[key] = nil
			lua_pushnil(L); // get 'new' first key
		}
	}
}

// Lua 5.1's checkudata throws an error on failure, we don't want that, we want NULL
void *checkudata(lua_State *L, int ud, const char *tname) noexcept {
	void *p = lua_touserdata(L, ud);
	if (p) { // value is a userdata?
		if (lua_getmetatable(L, ud)) { // does it have a metatable?
			lua_getfield(L, LUA_REGISTRYINDEX, tname); // get correct metatable
			if (lua_rawequal(L, -1, -2)) { // does it have correct mt?
				lua_pop(L, 2);
				return p;
			} else {
				lua_pop(L, 2);
			}
		}
	}
	return nullptr;
}

int cf_scite_send(lua_State *L) {
	// This is reinstated as a replacement for the old <pane>:send, which was removed
	// due to safety concerns.  Is now exposed as scite.SendEditor / scite.SendOutput.
	// It is rewritten to be typesafe, checking the arguments against the metadata in
	// IFaceTable in the same way that the object interface does.

	constexpr int paneIndex = lua_upvalueindex(1);
	check_pane_object(L, paneIndex);
	const int message = luaL_checkint(L, 1);

	lua_pushvalue(L, paneIndex);
	lua_replace(L, 1);

	IFaceFunction func = { "", 0, iface_void, {iface_void, iface_void} };
	for (int funcIdx = 0; funcIdx < IFaceTable::functionCount; ++funcIdx) {
		if (IFaceTable::functions[funcIdx].value == message) {
			func = IFaceTable::functions[funcIdx];
			break;
		}
	}

	if (func.value == 0) {
		for (int propIdx = 0; propIdx < IFaceTable::propertyCount; ++propIdx) {
			if (IFaceTable::properties[propIdx].getter == message) {
				func = IFaceTable::properties[propIdx].GetterFunction();
				break;
			} else if (IFaceTable::properties[propIdx].setter == message) {
				func = IFaceTable::properties[propIdx].SetterFunction();
				break;
			}
		}
	}

	if (func.value != 0) {
		if (IFaceFunctionIsScriptable(func)) {
			return iface_function_helper(L, func);
		} else {
			raise_error(L, "Cannot call send for this function: not scriptable.");
			return 0;
		}
	} else {
		raise_error(L, "Message number does not match any published Scintilla function or property");
		return 0;
	}
}

int cf_scite_constname(lua_State *L) {
	const int message = luaL_checkint(L, 1);
	const char *prefix = luaL_optstring(L, 2, nullptr);
	const std::string constName = IFaceTable::GetConstantName(message, prefix);
	if (constName.length() > 0) {
		push_string(L, constName);
		return 1;
	} else {
		raise_error(L, "Argument does not match any Scintilla / SciTE constant");
		return 0;
	}
}

int cf_scite_open(lua_State *L) {
	const char *s = luaL_checkstring(L, 1);
	if (s) {
		std::string cmd = "open:";
		cmd += s;
		Substitute(cmd, "\\", "\\\\");
		host->Perform(cmd.c_str());
	}
	return 0;
}

int cf_scite_reload_properties(lua_State *L) {
	if (!lua_gettop(L)) {
		host->Perform("reloadproperties:");
	}
	return 0;
}

int cf_scite_menu_command(lua_State *L) {
	const int cmdID = luaL_checkint(L, 1);
	if (cmdID) {
		host->DoMenuCommand(cmdID);
	}
	return 0;
}

int cf_scite_update_status_bar(lua_State *L) {
	const bool bUpdateSlowData = (lua_gettop(L) > 0 ? lua_toboolean(L, 1) : false) != 0;
	host->UpdateStatusBar(bUpdateSlowData);
	return 0;
}

int cf_scite_strip_show(lua_State *L) {
	const char *s = luaL_checkstring(L, 1);
	if (s) {
		host->UserStripShow(s);
	}
	return 0;
}

int cf_scite_strip_set(lua_State *L) {
	const int control = luaL_checkint(L, 1);
	const char *value = luaL_checkstring(L, 2);
	if (value) {
		host->UserStripSet(control, value);
	}
	return 0;
}

int cf_scite_strip_set_list(lua_State *L) {
	const int control = luaL_checkint(L, 1);
	const char *value = luaL_checkstring(L, 2);
	if (value) {
		host->UserStripSetList(control, value);
	}
	return 0;
}

int cf_scite_strip_value(lua_State *L) {
	const int control = luaL_checkint(L, 1);
	std::string value = host->UserStripValue(control);
	push_string(L, value);
	return 1;
}

ExtensionAPI::Pane check_pane_object(lua_State *L, int index) {
	ExtensionAPI::Pane *pPane = static_cast<ExtensionAPI::Pane *>(checkudata(L, index, "SciTE_MT_Pane"));

	if ((!pPane) && lua_istable(L, index)) {
		// so that nested objects have a convenient way to do a back reference
		const int absIndex = absolute_index(L, index);
		lua_pushliteral(L, "pane");
		lua_gettable(L, absIndex);
		pPane = static_cast<ExtensionAPI::Pane *>(checkudata(L, -1, "SciTE_MT_Pane"));
	}

	if (pPane) {
		if ((*pPane == ExtensionAPI::paneEditor) && (curBufferIndex < 0))
			raise_error(L, "Editor pane is not accessible at this time.");

		return *pPane;
	}

	if (index == 1)
		lua_pushliteral(L, "Self object is missing in pane method or property access.");
	else if (index == lua_upvalueindex(1))
		lua_pushliteral(L, "Internal error: pane object expected in closure.");
	else
		lua_pushliteral(L, "Pane object expected.");

	raise_error(L);
	return ExtensionAPI::paneOutput; // this line never reached
}

int cf_pane_textrange(lua_State *L) {
	const ExtensionAPI::Pane p = check_pane_object(L, 1);

	if (lua_gettop(L) >= 3) {
		const SA::Position cpMin = luaL_checkinteger(L, 2);
		const SA::Position cpMax = luaL_checkinteger(L, 3);
		if (cpMax >= 0) {
			std::string range = host->Range(p, SA::Span(cpMin, cpMax));
			push_string(L, range);
			return 1;
		} else {
			raise_error(L, "Invalid argument 2 for <pane>:textrange.  Positive number or zero expected.");
		}
	} else {
		raise_error(L, "Not enough arguments for <pane>:textrange");
	}

	return 0;
}

int cf_pane_insert(lua_State *L) {
	const ExtensionAPI::Pane p = check_pane_object(L, 1);
	const SA::Position pos = luaL_checkinteger(L, 2);
	const char *s = luaL_checkstring(L, 3);
	host->Insert(p, pos, s);
	return 0;
}

int cf_pane_remove(lua_State *L) {
	const ExtensionAPI::Pane p = check_pane_object(L, 1);
	const SA::Position cpMin = luaL_checkinteger(L, 2);
	const SA::Position cpMax = luaL_checkinteger(L, 3);
	host->Remove(p, cpMin, cpMax);
	return 0;
}

int cf_pane_append(lua_State *L) {
	const ExtensionAPI::Pane p = check_pane_object(L, 1);
	const char *s = luaL_checkstring(L, 2);
	host->Insert(p, host->PaneCaller(p).Length(), s);
	return 0;
}

int cf_pane_findtext(lua_State *L) {
	const ExtensionAPI::Pane p = check_pane_object(L, 1);

	const int nArgs = lua_gettop(L);

	const char *t = luaL_checkstring(L, 2);
	bool hasError = (!t);

	if (!hasError) {
		SA::Position rangeStart = 0;
		SA::Position rangeEnd = 0;

		const int flags = (nArgs > 2) ? luaL_checkint(L, 3) : 0;
		hasError = (flags == 0 && lua_gettop(L) > nArgs);

		if (!hasError) {
			if (nArgs > 3) {
				rangeStart = luaL_checkinteger(L, 4);
				hasError = (lua_gettop(L) > nArgs);
			}
		}

		SA::ScintillaCall &sc = host->PaneCaller(p);

		if (!hasError) {
			if (nArgs > 4) {
				rangeEnd = luaL_checkinteger(L, 5);
				hasError = (lua_gettop(L) > nArgs);
			} else {
				rangeEnd = sc.Length();
			}
		}

		if (!hasError) {
			sc.SetTargetRange(rangeStart, rangeEnd);
			sc.SetSearchFlags(static_cast<SA::FindOption>(flags));
			const SA::Span result = sc.SpanSearchInTarget(t);
			if (result.start >= 0) {
				lua_pushinteger(L, result.start);
				lua_pushinteger(L, result.end);
				return 2;
			} else {
				lua_pushnil(L);
				return 1;
			}
		}
	}

	if (hasError) {
		raise_error(L, "Invalid arguments for <pane>:findtext");
	}

	return 0;
}

// Pane match generator.  This was prototyped in about 30 lines of Lua.
// I hope the C++ version is more robust at least, e.g. prevents infinite
// loops and is more tamper-resistant.

struct PaneMatchObject {
	ExtensionAPI::Pane pane;
	SA::Span range;
	int flags; // this is really part of the state, but is kept here for convenience
	SA::Position endPosOrig; // has to do with preventing infinite loop on a 0-length match
	bool RangeValid() const noexcept {
		return (range.start >= 0) && (range.end >= 0) && (range.start <= range.end);
	}
};

int cf_match_replace(lua_State *L) {
	PaneMatchObject *pmo = static_cast<PaneMatchObject *>(checkudata(L, 1, "SciTE_MT_PaneMatchObject"));
	if (!pmo) {
		raise_error(L, "Self argument for match:replace() should be a pane match object.");
		return 0;
	} else if (!pmo->RangeValid()) {
		raise_error(L, "Blocked attempt to use invalidated pane match object.");
		return 0;
	}
	const char *replacement = luaL_checkstring(L, 2);

	// If an option were added to process \d back-references, it would just
	// be an optional boolean argument, i.e. m:replace([[\1]], true), and
	// this would just change ReplaceTarget to ReplaceTargetRE.
	// The problem is, even if SCFIND_REGEXP was used, it's hard to know
	// whether the back references are still valid.  So for now this is
	// left out.

	SA::ScintillaCall &sc = host->PaneCaller(pmo->pane);
	sc.SetTarget(pmo->range);
	sc.ReplaceTarget(lua_strlen(L, 2), replacement);
	pmo->range.end = sc.TargetEnd();
	return 0;
}

int cf_match_metatable_index(lua_State *L) {
	const PaneMatchObject *pmo = static_cast<PaneMatchObject *>(checkudata(L, 1, "SciTE_MT_PaneMatchObject"));
	if (!pmo) {
		raise_error(L, "Internal error: pane match object is missing.");
		return 0;
	} else if (!pmo->RangeValid()) {
		raise_error(L, "Blocked attempt to use invalidated pane match object.");
		return 0;
	}

	if (lua_isstring(L, 2)) {
		const char *key = lua_tostring(L, 2);

		if (0 == strcmp(key, "pos")) {
			lua_pushinteger(L, pmo->range.start);
			return 1;
		} else if (0 == strcmp(key, "len")) {
			lua_pushinteger(L, pmo->range.Length());
			return 1;
		} else if (0 == strcmp(key, "text")) {
			// If the document is changed while in the match loop, this will be broken.
			// Exception: if the changes are made exclusively through match:replace,
			// everything will be fine.
			const std::string range = host->Range(pmo->pane, pmo->range);
			push_string(L, range);
			return 1;
		} else if (0 == strcmp(key, "replace")) {
			constexpr int replaceMethodIndex = lua_upvalueindex(1);
			if (lua_iscfunction(L, replaceMethodIndex)) {
				lua_pushvalue(L, replaceMethodIndex);
				return 1;
			} else {
				return 0;
			}
		}
	}

	raise_error(L, "Invalid property / method name for pane match object.");
	return 0;
}

int cf_match_metatable_tostring(lua_State *L) {
	const PaneMatchObject *pmo = static_cast<PaneMatchObject *>(checkudata(L, 1, "SciTE_MT_PaneMatchObject"));
	if (!pmo) {
		raise_error(L, "Internal error: pane match object is missing.");
		return 0;
	} else if (!pmo->RangeValid()) {
		lua_pushliteral(L, "match(invalidated)");
		return 1;
	} else {
		lua_pushfstring(L, "match{pos=%d,len=%d}", pmo->range.start, pmo->range.Length());
		return 1;
	}
}

int cf_pane_match(lua_State *L) {
	const int nargs = lua_gettop(L);

	const ExtensionAPI::Pane p = check_pane_object(L, 1);
	luaL_checkstring(L, 2);

	constexpr int generatorIndex = lua_upvalueindex(1);
	if (!lua_isfunction(L, generatorIndex)) {
		raise_error(L, "Internal error: match generator is missing.");
		return 0;
	}

	lua_pushvalue(L, generatorIndex);

	// I'm putting some of the state in the match userdata for more convenient
	// access.  But, the search string is going in state because that part is
	// more convenient to leave in Lua form.
	lua_pushvalue(L, 2);

	PaneMatchObject *pmo = static_cast<PaneMatchObject *>(lua_newuserdata(L, sizeof(PaneMatchObject)));
	if (pmo) {
		pmo->pane = p;
		pmo->range = SA::Span(-1, 0);
		pmo->endPosOrig = 0;
		pmo->flags = 0;
		if (nargs >= 3) {
			pmo->flags = luaL_checkint(L, 3);
			if (nargs >= 4) {
				pmo->range.end = pmo->endPosOrig = luaL_checkinteger(L, 4);
				if (pmo->range.end < 0) {
					raise_error(L, "Invalid argument 3 for <pane>:match.  Positive number or zero expected.");
					return 0;
				}
			}
		}
		if (luaL_newmetatable(L, "SciTE_MT_PaneMatchObject")) {
			lua_pushliteral(L, "__index");
			lua_pushcfunction(L, cf_match_replace);
			lua_pushcclosure(L, cf_match_metatable_index, 1);
			lua_settable(L, -3);

			lua_pushliteral(L, "__tostring");
			lua_pushcfunction(L, cf_match_metatable_tostring);
			lua_settable(L, -3);
		}
		lua_setmetatable(L, -2);

		return 3;
	} else {
		raise_error(L, "Internal error: could not create match object.");
		return 0;
	}
}

int cf_pane_match_generator(lua_State *L) {
	const char *text = lua_tostring(L, 1);
	PaneMatchObject *pmo = static_cast<PaneMatchObject *>(checkudata(L, 2, "SciTE_MT_PaneMatchObject"));

	if (!(text)) {
		raise_error(L, "Internal error: invalid state for <pane>:match generator.");
		return 0;
	} else if (!pmo) {
		raise_error(L, "Internal error: invalid match object initializer for <pane>:match generator");
		return 0;
	}

	if ((pmo->range.end < 0) || (pmo->range.end < pmo->range.start)) {
		raise_error(L, "Blocked attempt to use invalidated pane match object.");
		return 0;
	}

	SA::Position searchPos = pmo->range.end;
	if ((pmo->range.start == pmo->endPosOrig) && (pmo->range.end == pmo->endPosOrig)) {
		// prevent infinite loop on zero-length match by stepping forward
		searchPos++;
	}

	SA::ScintillaCall &sc = host->PaneCaller(pmo->pane);

	const SA::Span range(searchPos, sc.Length());

	if (range.end > range.start) {
		sc.SetTarget(range);
		sc.SetSearchFlags(static_cast<SA::FindOption>(pmo->flags));
		const SA::Span result = sc.SpanSearchInTarget(text);
		if (result.start >= 0) {
			pmo->range = result;
			pmo->endPosOrig = result.end;
			lua_pushvalue(L, 2);
			return 1;
		}
	}

	// One match object is used throughout the entire iteration.
	// This means it's bad to try to save the match object for later
	// reference.
	pmo->range.start = pmo->range.end = pmo->endPosOrig = -1;
	lua_pushnil(L);
	return 1;
}

int cf_props_metatable_index(lua_State *L) {
	const int selfArg = lua_isuserdata(L, 1) ? 1 : 0;

	if (lua_isstring(L, selfArg + 1)) {
		std::string value = host->Property(lua_tostring(L, selfArg + 1));
		push_string(L, value);
		return 1;
	} else {
		raise_error(L, "String argument required for property access");
	}
	return 0;
}

int cf_props_metatable_newindex(lua_State *L) {
	const int selfArg = lua_isuserdata(L, 1) ? 1 : 0;

	const char *key = lua_isstring(L, selfArg + 1) ? lua_tostring(L, selfArg + 1) : nullptr;
	const char *val = lua_tostring(L, selfArg + 2);

	if (key && *key) {
		if (val) {
			host->SetProperty(key, val);
		} else if (lua_isnil(L, selfArg + 2)) {
			host->UnsetProperty(key);
		} else {
			raise_error(L, "Expected string or nil for property assignment.");
		}
	} else {
		raise_error(L, "Property name must be a non-empty string.");
	}
	return 0;
}

/*
int cf_os_execute(lua_State *L) {
	// The SciTE version of os.execute would pipe its stdout and stderr
	// to the output pane.  This can be implemented in terms of popen
	// on GTK and in terms of CreateProcess on Windows.  Either way,
	// stdin should be null, and the Lua script should wait for the
	// subprocess to finish before continuing.  (What if it takes
	// a very long time?  Timeout?)

	raise_error(L, "Not implemented.");
	return 0;
}
*/

int cf_global_print(lua_State *L) {
	const int nargs = lua_gettop(L);

	lua_getglobal(L, "tostring");

	for (int i = 1; i <= nargs; ++i) {
		if (i > 1)
			host->Trace("\t");

		const char *argStr = lua_tostring(L, i);
		if (argStr) {
			host->Trace(argStr);
		} else {
			lua_pushvalue(L, -1); // tostring
			lua_pushvalue(L, i);
			lua_call(L, 1, 1);
			argStr = lua_tostring(L, -1);
			if (argStr) {
				host->Trace(argStr);
			} else {
				raise_error(L, "tostring (called from print) returned a non-string");
			}
			lua_settop(L, nargs + 1);
		}
	}

	host->Trace("\n");
	return 0;
}


int cf_global_trace(lua_State *L) {
	const char *s = lua_tostring(L, 1);
	if (s) {
		host->Trace(s);
	}
	return 0;
}

int cf_global_dostring(lua_State *L) {
	const int nargs = lua_gettop(L);
	const char *code = luaL_checkstring(L, 1);
	const char *name = luaL_optstring(L, 2, code);
	if (0 == luaL_loadbuffer(L, code, lua_strlen(L, 1), name)) {
		lua_call(L, 0, LUA_MULTRET);
		return lua_gettop(L) - nargs;
	} else {
		raise_error(L);
	}
	return 0;
}

bool call_function(lua_State *L, int nargs, bool ignoreFunctionReturnValue=false) {
	bool handled = false;
	if (L) {
		int traceback = 0;
		if (tracebackEnabled) {
			lua_getglobal(L, "debug");
			lua_getfield(L, -1, "traceback");
			lua_remove(L, -2);
			if (lua_isfunction(L, -1)) {
				traceback = lua_gettop(L) - nargs - 1;
				lua_insert(L, traceback);
			} else {
				lua_pop(L, 1);
			}
		}

		const int result = lua_pcall(L, nargs, ignoreFunctionReturnValue ? 0 : 1, traceback);

		if (traceback) {
			lua_remove(L, traceback);
		}

		if (0 == result) {
			if (ignoreFunctionReturnValue) {
				handled = true;
			} else {
				handled = (0 != lua_toboolean(L, -1));
				lua_pop(L, 1);
			}
		} else if (result == LUA_ERRRUN) {
			lua_getglobal(L, "print");
			lua_insert(L, -2); // use pushed error message
			lua_pcall(L, 1, 0, 0);
		} else {
			lua_pop(L, 1);
			if (result == LUA_ERRMEM) {
				host->Trace("> Lua: memory allocation error\n");
			} else if (result == LUA_ERRERR) {
				host->Trace("> Lua: an error occurred, but cannot be reported due to failure in _TRACEBACK\n");
			} else {
				host->Trace("> Lua: unexpected error\n");
			}
		}
	}
	return handled;
}

bool HasNamedFunction(const char *name) noexcept {
	bool hasFunction = false;
	if (luaState) {
		hasFunction = lua_getglobal(luaState, name) != LUA_TNIL;
		lua_pop(luaState, 1);
	}
	return hasFunction;
}

bool CallNamedFunction(const char *name) {
	bool handled = false;
	if (luaState) {
		if (lua_getglobal(luaState, name) != LUA_TNIL) {
			handled = call_function(luaState, 0);
		} else {
			lua_pop(luaState, 1);
		}
	}
	return handled;
}

bool CallNamedFunction(const char *name, const char *arg) {
	bool handled = false;
	if (luaState) {
		if (lua_getglobal(luaState, name) != LUA_TNIL) {
			lua_pushstring(luaState, arg);
			handled = call_function(luaState, 1);
		} else {
			lua_pop(luaState, 1);
		}
	}
	return handled;
}

bool CallNamedFunction(const char *name, intptr_t numberArg, const char *stringArg) {
	bool handled = false;
	if (luaState) {
		if (lua_getglobal(luaState, name) != LUA_TNIL) {
			lua_pushinteger(luaState, numberArg);
			lua_pushstring(luaState, stringArg);
			handled = call_function(luaState, 2);
		} else {
			lua_pop(luaState, 1);
		}
	}
	return handled;
}

bool CallNamedFunction(const char *name, intptr_t numberArg, intptr_t numberArg2) {
	bool handled = false;
	if (luaState) {
		if (lua_getglobal(luaState, name) != LUA_TNIL) {
			lua_pushinteger(luaState, numberArg);
			lua_pushinteger(luaState, numberArg2);
			handled = call_function(luaState, 2);
		} else {
			lua_pop(luaState, 1);
		}
	}
	return handled;
}

int iface_function_helper(lua_State *L, const IFaceFunction &func) {
	const ExtensionAPI::Pane p = check_pane_object(L, 1);

	int arg = 2;

	intptr_t params[2] = {0, 0};

	std::string stringResult;
	bool needStringResult = false;

	int loopParamCount = 2;

	if (func.paramType[0] == iface_length && func.paramType[1] == iface_string) {
		params[0] = lua_strlen(L, arg);
		params[1] = SptrFromString(params[0] ? lua_tostring(L, arg) : "");
		loopParamCount = 0;
	} else if ((func.paramType[1] == iface_stringresult) || (func.returnType == iface_stringresult)) {
		needStringResult = true;
		// The buffer will be allocated later, so it won't leak if Lua does
		// a longjmp in response to a bad arg.
		if (func.paramType[0] == iface_length) {
			loopParamCount = 0;
		} else {
			loopParamCount = 1;
		}
	}

	for (int i=0; i<loopParamCount; ++i) {
		if (func.paramType[i] == iface_string) {
			const char *s = lua_tostring(L, arg++);
			params[i] = SptrFromString(s ? s : "");
		} else if (func.paramType[i] == iface_keymod) {
			const int keycode = luaL_checkint(L, arg++) & 0xFFFF;
			const intptr_t modifiers = luaL_checkint(L, arg++) &
					      static_cast<int>(SA::KeyMod::Shift|SA::KeyMod::Ctrl|SA::KeyMod::Alt);
			params[i] = keycode | (modifiers<<16);
		} else if (func.paramType[i] == iface_bool) {
			params[i] = lua_toboolean(L, arg++);
		} else if (IFaceTypeIsNumeric(func.paramType[i])) {
			params[i] = luaL_checkinteger(L, arg++);
		}
	}

	if (needStringResult) {
		const intptr_t stringResultLen = host->Send(p, static_cast<SA::Message>(func.value), params[0], 0);
		stringResult.assign(stringResultLen, '\0');
		params[1] = SptrFromPointer(stringResult.data());
		if (func.paramType[0] == iface_length) {
			params[0] = stringResultLen;
		}
	}

	// Now figure out what to do with the param types and return type.
	// - stringresult gets inserted at the start of return tuple.
	// - numeric return type gets returned to lua as a number (following the stringresult)
	// - other return types e.g. void get dropped.

	intptr_t result = 0;
	try {
		result = host->Send(p, static_cast<SA::Message>(func.value), params[0], params[1]);
	} catch (const SA::Failure &sf) {
		std::string failureExplanation;
		failureExplanation += ">Lua: Scintilla failure ";
		failureExplanation += StdStringFromInteger(static_cast<int>(sf.status));
		failureExplanation += " for message ";
		failureExplanation += StdStringFromInteger(func.value);
		failureExplanation += ".\n";
		// Reset status before continuing
		host->PaneCaller(p).SetStatus(SA::Status::Ok);
		host->Trace(failureExplanation.c_str());
	}

	int resultCount = 0;

	if (needStringResult) {
		push_string(L, stringResult);
		resultCount++;
	}

	if (func.returnType == iface_bool) {
		lua_pushboolean(L, static_cast<int>(result));
		resultCount++;
	} else if (IFaceTypeIsNumeric(func.returnType)) {
		lua_pushinteger(L, result);
		resultCount++;
	}

	return resultCount;
}

struct IFacePropertyBinding {
	ExtensionAPI::Pane pane;
	const IFaceProperty *prop;
};

int cf_ifaceprop_metatable_index(lua_State *L) {
	// if there is a getter, __index calls it
	// otherwise, __index raises "property 'name' is write-only".
	const IFacePropertyBinding *ipb = static_cast<IFacePropertyBinding *>(checkudata(L, 1, "SciTE_MT_IFacePropertyBinding"));
	if (!(ipb && IFacePropertyIsScriptable(*(ipb->prop)))) {
		raise_error(L, "Internal error: property binding is improperly set up");
		return 0;
	}
	if (ipb->prop->getter == 0) {
		raise_error(L, "Attempt to read a write-only indexed property");
		return 0;
	}
	const IFaceFunction func = ipb->prop->GetterFunction();

	// rewrite the stack to match what the function expects.  put pane at index 1; param is already at index 2.
	push_pane_object(L, ipb->pane);
	lua_replace(L, 1);
	lua_settop(L, 2);
	return iface_function_helper(L, func);
}

int cf_ifaceprop_metatable_newindex(lua_State *L) {
	const IFacePropertyBinding *ipb = static_cast<IFacePropertyBinding *>(checkudata(L, 1, "SciTE_MT_IFacePropertyBinding"));
	if (!(ipb && IFacePropertyIsScriptable(*(ipb->prop)))) {
		raise_error(L, "Internal error: property binding is improperly set up");
		return 0;
	}
	if (ipb->prop->setter == 0) {
		raise_error(L, "Attempt to write a read-only indexed property");
		return 0;
	}
	const IFaceFunction func = ipb->prop->SetterFunction();

	// rewrite the stack to match what the function expects.
	// pane at index 1; param at index 2, value at index 3
	push_pane_object(L, ipb->pane);
	lua_replace(L, 1);
	lua_settop(L, 3);
	return iface_function_helper(L, func);
}

int cf_pane_iface_function(lua_State *L) {
	constexpr int funcidx = lua_upvalueindex(1);
	const IFaceFunction *func = static_cast<IFaceFunction *>(lua_touserdata(L, funcidx));
	if (func) {
		return iface_function_helper(L, *func);
	} else {
		raise_error(L, "Internal error - bad upvalue in iface function closure");
		return 0;
	}
}

int push_iface_function(lua_State *L, const char *name) {
	int i = IFaceTable::FindFunction(name);
	if (i >= 0) {
		if (IFaceFunctionIsScriptable(IFaceTable::functions[i])) {
			lua_pushlightuserdata(L, const_cast<IFaceFunction *>(IFaceTable::functions+i));
			lua_pushcclosure(L, cf_pane_iface_function, 1);

			// Since Lua experts say it is inefficient to create closures / cfunctions
			// in an inner loop, I tried caching the closures in the metatable, and looking
			// for them there first.  However, it made very little difference and did not
			// seem worth the added complexity. - WBD

			return 1;
		}
	}
	return -1; // signal to try next pane index handler
}

int push_iface_propval(lua_State *L, const char *name) {
	// this function doesn't raise errors, but returns 0 if the function is not handled.

	const int propidx = IFaceTable::FindProperty(name);
	if (propidx >= 0) {
		const IFaceProperty &prop = IFaceTable::properties[propidx];
		if (!IFacePropertyIsScriptable(prop)) {
			raise_error(L, "Error: iface property is not scriptable.");
			return -1;
		}

		if (prop.paramType == iface_void) {
			if (prop.getter) {
				lua_settop(L, 1);
				return iface_function_helper(L, prop.GetterFunction());
			}
		} else if (prop.paramType == iface_bool) {
			// The bool getter is untested since there are none in the iface.
			// However, the following is suggested as a reference protocol.
			const ExtensionAPI::Pane p = check_pane_object(L, 1);

			if (prop.getter) {
				if (host->Send(p, static_cast<SA::Message>(prop.getter), 1, 0)) {
					lua_pushnil(L);
					return 1;
				} else {
					lua_settop(L, 1);
					lua_pushboolean(L, 0);
					return iface_function_helper(L, prop.GetterFunction());
				}
			}
		} else {
			// Indexed property.  These return an object with the following behaviour:
			// if there is a getter, __index calls it
			// otherwise, __index raises "property 'name' is write-only".
			// if there is a setter, __newindex calls it
			// otherwise, __newindex raises "property 'name' is read-only"

			IFacePropertyBinding *ipb = static_cast<IFacePropertyBinding *>(lua_newuserdata(L, sizeof(IFacePropertyBinding)));
			if (ipb) {
				ipb->pane = check_pane_object(L, 1);
				ipb->prop = &prop;
				if (luaL_newmetatable(L, "SciTE_MT_IFacePropertyBinding")) {
					lua_pushliteral(L, "__index");
					lua_pushcfunction(L, cf_ifaceprop_metatable_index);
					lua_settable(L, -3);
					lua_pushliteral(L, "__newindex");
					lua_pushcfunction(L, cf_ifaceprop_metatable_newindex);
					lua_settable(L, -3);
				}
				lua_setmetatable(L, -2);
				return 1;
			} else {
				raise_error(L, "Internal error: failed to allocate userdata for indexed property");
				return -1;
			}
		}
	}

	return -1; // signal to try next pane index handler
}

int cf_pane_metatable_index(lua_State *L) {
	if (lua_isstring(L, 2)) {
		const char *name = lua_tostring(L, 2);

		// these return the number of values pushed (possibly 0), or -1 if no match
		int results = push_iface_function(L, name);
		if (results < 0)
			results = push_iface_propval(L, name);

		if (results >= 0) {
			return results;
		} else if (name[0] != '_') {
			lua_getmetatable(L, 1);
			if (lua_istable(L, -1)) {
				lua_pushvalue(L, 2);
				lua_gettable(L, -2);
				if (!lua_isnil(L, -1))
					return 1;
			}
		}
	}

	raise_error(L, "Pane function / readable property / indexed writable property name expected");
	return 0;
}

int cf_pane_metatable_newindex(lua_State *L) {
	if (lua_isstring(L, 2)) {
		const int propidx = IFaceTable::FindProperty(lua_tostring(L, 2));
		if (propidx >= 0) {
			const IFaceProperty &prop = IFaceTable::properties[propidx];
			if (IFacePropertyIsScriptable(prop)) {
				if (prop.setter) {
					// stack needs to be rearranged to look like an iface function call
					lua_remove(L, 2);

					if (prop.paramType == iface_void) {
						return iface_function_helper(L, prop.SetterFunction());

					} else if ((prop.paramType == iface_bool)) {
						if (!lua_isnil(L, 3)) {
							lua_pushboolean(L, 1);
							lua_insert(L, 2);
						} else {
							// the nil will do as a false value.
							// just push an arbitrary numeric value that Scintilla will ignore
							lua_pushinteger(L, 0);
						}
						return iface_function_helper(L, prop.SetterFunction());

					} else {
						raise_error(L, "Error - (pane object) cannot assign directly to indexed property");
					}
				} else {
					raise_error(L, "Error - (pane object) cannot assign to a read-only property");
				}
			}
		}
	}

	raise_error(L, "Error - (pane object) expected the name of a writable property");
	return 0;
}

void push_pane_object(lua_State *L, ExtensionAPI::Pane p) noexcept {
	*static_cast<ExtensionAPI::Pane *>(lua_newuserdata(L, sizeof(p))) = p;
	if (luaL_newmetatable(L, "SciTE_MT_Pane")) {
		lua_pushcfunction(L, cf_pane_metatable_index);
		lua_setfield(L, -2, "__index");
		lua_pushcfunction(L, cf_pane_metatable_newindex);
		lua_setfield(L, -2, "__newindex");

		// Push built-in functions into the metatable, where the custom
		// __index metamethod will find them.

		lua_pushcfunction(L, cf_pane_findtext);
		lua_setfield(L, -2, "findtext");
		lua_pushcfunction(L, cf_pane_textrange);
		lua_setfield(L, -2, "textrange");
		lua_pushcfunction(L, cf_pane_insert);
		lua_setfield(L, -2, "insert");
		lua_pushcfunction(L, cf_pane_remove);
		lua_setfield(L, -2, "remove");
		lua_pushcfunction(L, cf_pane_append);
		lua_setfield(L, -2, "append");

		lua_pushcfunction(L, cf_pane_match_generator);
		lua_pushcclosure(L, cf_pane_match, 1);
		lua_setfield(L, -2, "match");
	}
	lua_setmetatable(L, -2);
}

int cf_global_metatable_index(lua_State *L) {
	if (lua_isstring(L, 2)) {
		const char *name = lua_tostring(L, 2);
		if ((name[0] < 'A') || (name[0] > 'Z') || ((name[1] >= 'a') && (name[1] <= 'z'))) {
			// short circuit; iface constants are always upper-case and start with a letter
			return 0;
		}

		int i = IFaceTable::FindConstant(name);
		if (i >= 0) {
			lua_pushinteger(L, IFaceTable::constants[i].value);
			return 1;
		} else {
			i = IFaceTable::FindFunctionByConstantName(name);
			if (i >= 0) {
				lua_pushinteger(L, IFaceTable::functions[i].value);

				// FindFunctionByConstantName is slow, so cache the result into the
				// global table.  My tests show this gives an order of magnitude
				// improvement.
				lua_pushvalue(L, 2);
				lua_pushvalue(L, -2);
				lua_rawset(L, 1);

				return 1;
			}
		}
	}

	return 0; // global namespace access should not raise errors
}

int LuaPanicFunction(lua_State *L) {
	if (L == luaState) {
		lua_close(luaState);
		luaState = nullptr;
		luaDisabled = true;
	}
	host->Trace("\n> Lua: error occurred in unprotected call.  This is very bad.\n");
	return 1;
}

// Don't initialise Lua in LuaExtension::Initialise.  Wait and initialise Lua the
// first time Lua is used, e.g. when a Load event is called with an argument that
// appears to be the name of a Lua script.  This just-in-time initialisation logic
// does add a little extra complexity but not a lot.  It's probably worth it,
// since it means a user who is having trouble with Lua can just refrain from
// using it.

bool CheckStartupScript() {
	startupScript = host->Property("ext.lua.startup.script");
	return startupScript.length() > 0;
}

void PublishGlobalBufferData() noexcept {
// release 1.62
// A Lua table called 'buffer' is associated with each buffer
// and can be used to maintain buffer-specific state.
	if (curBufferIndex >= 0) {
		lua_pushliteral(luaState, "SciTE_BufferData_Array");
		lua_rawget(luaState, LUA_REGISTRYINDEX);
		// Create new SciTE_BufferData_Array / append to LUA_REGISTRYINDEX
		if (!lua_istable(luaState, -1)) {
			lua_pop(luaState, 1);
			lua_newtable(luaState);
			lua_pushliteral(luaState, "SciTE_BufferData_Array");
			lua_pushvalue(luaState, -2);
			lua_rawset(luaState, LUA_REGISTRYINDEX);
		}
		//  create new entry for current buffer in SciTE_BufferData_Array(idx)
		lua_rawgeti(luaState, -1, curBufferIndex);
		if (!lua_istable(luaState, -1)) {
			lua_pop(luaState, 1);
			lua_newtable(luaState);
			// remember it
			lua_pushvalue(luaState, -1);
			lua_rawseti(luaState, -3, curBufferIndex);
		}
		// replace SciTE_BufferData_Array on the Stack (Leaving (buffer=-1, 'buffer'=-2))
		// done to apply the expanded  SciTE_BufferData_Array ?
		lua_replace(luaState, -2);
	} else {
		/// ensure that the luatable "buffer" will be empty during startup and before any InitBuffer / ActivateBuffer
		lua_pushnil(luaState);
	}
	lua_setglobal(luaState, "buffer");
}

bool InitGlobalScope(bool checkProperties, bool forceReload = false) {
	bool reload = forceReload;
	if (checkProperties) {
		const int resetMode = GetPropertyInt("ext.lua.reset");
		if (resetMode >= 1) {
			reload = true;
		}
	}

	tracebackEnabled = (GetPropertyInt("ext.lua.debug.traceback") == 1);

	if (luaState) {
		// The Clear / Load used to use metatables to setup without having to re-run the scripts,
		// but this was unreliable e.g. a few library functions and some third-party code use
		// rawget to access functions in the global scope.  So the new method makes a shallow
		// copy of the initialized global environment, and uses that to re-init the scope.

		if (!reload) {
			lua_pushglobaltable(luaState);
			lua_getfield(luaState, LUA_REGISTRYINDEX, "SciTE_InitialState");
			if (lua_istable(luaState, -1)) {
				clear_table(luaState, -2, true);
				merge_table(luaState, -2, -1, true);
				lua_pop(luaState, 2);

				// restore initial package.loaded state
				lua_getfield(luaState, LUA_REGISTRYINDEX, "SciTE_InitialPackageState");
				lua_getfield(luaState, LUA_REGISTRYINDEX, "_LOADED");
				clear_table(luaState, -1, false);
				merge_table(luaState, -1, -2, false);
				lua_pop(luaState, 2);

				PublishGlobalBufferData();

				return true;
			} else {
				lua_pop(luaState, 1);
			}
		}

		// reload mode is enabled, or else the initial state has been broken.
		// either way, we're going to need a "new" initial state.

		lua_pushnil(luaState);
		lua_setfield(luaState, LUA_REGISTRYINDEX, "SciTE_InitialState");

		// Also reset buffer data, since scripts might depend on this to know
		// whether they need to re-initialize something.
		lua_pushnil(luaState);
		lua_setfield(luaState, LUA_REGISTRYINDEX, "SciTE_BufferData_Array");

		// Don't replace global scope using new_table, because then startup script is
		// bound to a different copy of the globals than the extension script.

		lua_pushglobaltable(luaState);
		clear_table(luaState, -1, true);
		lua_pop(luaState, 1);

		// Lua 5.1: _LOADED is in LUA_REGISTRYINDEX, so it must be cleared before
		// loading libraries or they will not load because Lua's package system
		// thinks they are already loaded
		lua_pushnil(luaState);
		lua_setfield(luaState, LUA_REGISTRYINDEX, "_LOADED");

	} else if (!luaDisabled) {
		luaState = luaL_newstate();
		if (!luaState) {
			luaDisabled = true;
			host->Trace("> Lua: scripting engine failed to initialise\n");
			return false;
		}
		lua_atpanic(luaState, LuaPanicFunction);

	} else {
		return false;
	}

	// ...register standard libraries
	luaL_openlibs(luaState);

	lua_register(luaState, "_ALERT", cf_global_print);

	// although this is mostly redundant with output:append
	// it is still included for now
	lua_register(luaState, "trace", cf_global_trace);

	// emulate a Lua 4 function that is useful in menu commands
	lua_register(luaState, "dostring", cf_global_dostring);

	// override a library function whose default impl uses stdout
	lua_register(luaState, "print", cf_global_print);

	// props object - provides access to Property and SetProperty
	lua_newuserdata(luaState, 1); // the value doesn't matter.
	if (luaL_newmetatable(luaState, "SciTE_MT_Props")) {
		lua_pushcfunction(luaState, cf_props_metatable_index);
		lua_setfield(luaState, -2, "__index");
		lua_pushcfunction(luaState, cf_props_metatable_newindex);
		lua_setfield(luaState, -2, "__newindex");
	}
	lua_setmetatable(luaState, -2);
	lua_setglobal(luaState, "props");

	// pane objects
	push_pane_object(luaState, ExtensionAPI::paneEditor);
	lua_setglobal(luaState, "editor");

	push_pane_object(luaState, ExtensionAPI::paneOutput);
	lua_setglobal(luaState, "output");

	// scite
	lua_newtable(luaState);

	lua_getglobal(luaState, "editor");
	lua_pushcclosure(luaState, cf_scite_send, 1);
	lua_setfield(luaState, -2, "SendEditor");

	lua_getglobal(luaState, "output");
	lua_pushcclosure(luaState, cf_scite_send, 1);
	lua_setfield(luaState, -2, "SendOutput");

	lua_pushcfunction(luaState, cf_scite_constname);
	lua_setfield(luaState, -2, "ConstantName");

	lua_pushcfunction(luaState, cf_scite_open);
	lua_setfield(luaState, -2, "Open");

	lua_pushcfunction(luaState, cf_scite_reload_properties);
	lua_setfield(luaState, -2, "ReloadProperties");

	lua_pushcfunction(luaState, cf_scite_menu_command);
	lua_setfield(luaState, -2, "MenuCommand");

	lua_pushcfunction(luaState, cf_scite_update_status_bar);
	lua_setfield(luaState, -2, "UpdateStatusBar");

	lua_pushcfunction(luaState, cf_scite_strip_show);
	lua_setfield(luaState, -2, "StripShow");

	lua_pushcfunction(luaState, cf_scite_strip_set);
	lua_setfield(luaState, -2, "StripSet");

	lua_pushcfunction(luaState, cf_scite_strip_set_list);
	lua_setfield(luaState, -2, "StripSetList");

	lua_pushcfunction(luaState, cf_scite_strip_value);
	lua_setfield(luaState, -2, "StripValue");

	lua_setglobal(luaState, "scite");

	// append a Metatable onto global namespace, to publish iface constants
	lua_pushglobaltable(luaState);
	if (luaL_newmetatable(luaState, "SciTE_MT_GlobalScope")) {
		lua_pushcfunction(luaState, cf_global_metatable_index);
		lua_setfield(luaState, -2, "__index");
	}

	lua_setmetatable(luaState, -2);
	lua_pop(luaState, 1);

	if (checkProperties && reload) {
		CheckStartupScript();
	}

	if (startupScript.length()) {
		// TODO: Should buffer be deactivated temporarily, so editor iface
		// functions won't be available during a reset, just as they are not
		// available during a normal startup?  Are there any other functions
		// that should be blocked during startup, e.g. the ones that allow
		// you to add or switch buffers?

		FilePath fpTest(GUI::StringFromUTF8(startupScript));
		if (fpTest.Exists()) {
			if (0 == luaL_loadfile(luaState, startupScript.c_str())) {
				if (!call_function(luaState, 0, true)) {
					host->Trace(">Lua: error occurred while running startup script\n");
				}
			} else {
				host->Trace(lua_tostring(luaState, -1));
				host->Trace("\n>Lua: error occurred while loading startup script\n");
				lua_pop(luaState, 1);
			}
		}
	}

	// Clone the initial (globalsindex) state (including metatable) in the registry so that it can be restored.
	// (If reset==1 this will not be used, but this is a shallow copy, not very expensive, and
	// who knows what the value of reset will be the next time InitGlobalScope runs.)

	lua_pushglobaltable(luaState);
	clone_table(luaState, -1, true);
	lua_setfield(luaState, LUA_REGISTRYINDEX, "SciTE_InitialState");
	lua_pop(luaState, 1);

	// Clone loaded packages (package.loaded) state in the registry so that it can be restored.
	lua_getfield(luaState, LUA_REGISTRYINDEX, "_LOADED");
	clone_table(luaState, -1);
	lua_setfield(luaState, LUA_REGISTRYINDEX, "SciTE_InitialPackageState");
	lua_pop(luaState, 1);

	PublishGlobalBufferData();

	return true;
}

}

bool LuaExtension::Initialise(ExtensionAPI *host_) {
	host = host_;

	if (CheckStartupScript()) {
		InitGlobalScope(false);
	}

	return false;
}

bool LuaExtension::Finalise() noexcept {
	if (luaState) {
		lua_close(luaState);
	}

	luaState = nullptr;
	host = nullptr;

	// The rest don't strictly need to be cleared since they
	// are never accessed except when luaState and host are set

	startupScript.clear();

	return false;
}

bool LuaExtension::Clear() {
	if (luaState) {
		CallNamedFunction("OnClear");
	}
	if (luaState) {
		InitGlobalScope(true);
		extensionScript.clear();
	} else if ((GetPropertyInt("ext.lua.reset") >= 1) && CheckStartupScript()) {
		InitGlobalScope(false);
	}
	return false;
}

bool LuaExtension::Load(const char *filename) {
	bool loaded = false;

	if (!luaDisabled) {
		const size_t sl = strlen(filename);
		if (sl >= 4 && strcmp(filename+sl-4, ".lua")==0) {
			if (luaState || InitGlobalScope(false)) {
				extensionScript = filename;
				luaL_loadfile(luaState, extensionScript.c_str());
				if (!call_function(luaState, 0, true)) {
					host->Trace(">Lua: error occurred while loading extension script\n");
				}
				loaded = true;
			}
		}
	}
	return loaded;
}


bool LuaExtension::InitBuffer(int index) {
	if (index > maxBufferIndex)
		maxBufferIndex = index;

	if (luaState) {
		// This buffer might be recycled.  Clear the data associated
		// with the old file.

		lua_getfield(luaState, LUA_REGISTRYINDEX, "SciTE_BufferData_Array");
		if (lua_istable(luaState, -1)) {
			lua_pushnil(luaState);
			lua_rawseti(luaState, -2, index);
		}
		lua_pop(luaState, 1);
		// We also need to handle cases where Lua initialization is
		// delayed (e.g. no startup script).  For that we'll just
		// explicitly call InitBuffer(curBufferIndex)
	}

	curBufferIndex = index;

	return false;
}

bool LuaExtension::ActivateBuffer(int index) {
	// Probably don't need to do anything with Lua here.  Setting
	// curBufferIndex is important so that InitGlobalScope knows
	// which buffer is active, in order to populate the 'buffer'
	// global with the right data.

	curBufferIndex = index;

	return false;
}

bool LuaExtension::RemoveBuffer(int index) {
	if (luaState) {
		// Remove the bufferdata element at index, and move
		// the other elements down.  The element at the
		// current maxBufferIndex can be discarded after
		// it gets copied to maxBufferIndex-1.

		lua_getfield(luaState, LUA_REGISTRYINDEX, "SciTE_BufferData_Array");
		if (lua_istable(luaState, -1)) {
			for (lua_Integer i = index; i < maxBufferIndex; ++i) {
				lua_rawgeti(luaState, -1, i+1);
				lua_rawseti(luaState, -2, i);
			}

			lua_pushnil(luaState);
			lua_rawseti(luaState, -2, maxBufferIndex);

			lua_pop(luaState, 1); // the bufferdata table
		} else {
			lua_pop(luaState, 1);
		}
	}

	if (maxBufferIndex > 0)
		maxBufferIndex--;

	// Invalidate current buffer index; Activate or Init will follow.
	curBufferIndex = -1;

	return false;
}

bool LuaExtension::OnExecute(const char *s) {
// gets called when selecting a luaScript within the tools menu
// pcalls string.find(s) -> if that succeeds, insert the function onto the stack and try to call_function(s).
	bool handled = false;

	if (luaState || InitGlobalScope(false)) {
		// May as well use Lua's pattern matcher to parse the command.
		// Scintilla's RESearch was the other option.
		const int stackBase = lua_gettop(luaState);
		lua_pushglobaltable(luaState);
		lua_pushliteral(luaState, "string");
		lua_rawget(luaState, -2);
		if (lua_istable(luaState, -1)) {
			lua_pushliteral(luaState, "find");
			lua_rawget(luaState, -2);
			if (lua_isfunction(luaState, -1)) {
				lua_pushstring(luaState, s);
				lua_pushliteral(luaState, "^%s*([%a_][%a%d_]*)%s*(.-)%s*$");
				const int status = lua_pcall(luaState, 2, 4, 0);
				if (status==0) {
					lua_insert(luaState, stackBase+1);	//function
					lua_gettable(luaState, LUA_GLOBALSINDEX);
					if (!lua_isnil(luaState, -1)) {
						if (lua_isfunction(luaState, -1)) {
							// Try calling it and, even if it fails, short-circuit Filerx
							handled = true;
							lua_insert(luaState, stackBase+1);
							lua_settop(luaState, stackBase+2);
							if (!call_function(luaState, 1, true)) {
								std::string traceMessage = "> Lua: error occurred while processing command '";
								traceMessage += s;
								traceMessage += "'\n";
								host->Trace(traceMessage.c_str());
							}
						} else {
							std::string traceMessage = "> Lua: this expression is not a function '";
							traceMessage += s;
							traceMessage += "'\n";
							host->Trace(traceMessage.c_str());
						}
					} else {
						std::string traceMessage = "> Lua: error checking global scope for command '";
						traceMessage += s;
						traceMessage += "'\n";
						host->Trace(traceMessage.c_str());
					}
				}
			}
		} else {
			host->Trace("> Lua: string library not loaded\n");
		}
		lua_settop(luaState, stackBase);
	}

	return handled;
}

bool LuaExtension::OnOpen(const char *filename) {
	return CallNamedFunction("OnOpen", filename);
}

bool LuaExtension::OnSwitchFile(const char *filename) {
	return CallNamedFunction("OnSwitchFile", filename);
}

bool LuaExtension::OnBeforeSave(const char *filename) {
	return CallNamedFunction("OnBeforeSave", filename);
}

bool LuaExtension::OnSave(const char *filename) {
	const bool result = CallNamedFunction("OnSave", filename);

	FilePath fpSaving = FilePath(GUI::StringFromUTF8(filename)).NormalizePath();
	if (startupScript.length() && fpSaving == FilePath(GUI::StringFromUTF8(startupScript)).NormalizePath()) {
		if (GetPropertyInt("ext.lua.auto.reload") > 0) {
			InitGlobalScope(false, true);
			if (extensionScript.length()) {
				Load(extensionScript.c_str());
			}
		}
	} else if (extensionScript.length() && 0 == strcmp(filename, extensionScript.c_str())) {
		if (GetPropertyInt("ext.lua.auto.reload") > 0) {
			InitGlobalScope(false, false);
			Load(extensionScript.c_str());
		}
	}

	return result;
}

bool LuaExtension::OnChar(char ch) {
	const char chs[2] = {ch, '\0'};
	return CallNamedFunction("OnChar", chs);
}

bool LuaExtension::OnSavePointReached() {
	return CallNamedFunction("OnSavePointReached");
}

bool LuaExtension::OnSavePointLeft() {
	return CallNamedFunction("OnSavePointLeft");
}

// Similar to StyleContext class in Scintilla
struct StylingContext {
	SA::Position startPos;
	SA::Position lengthDoc;
	int initStyle;
	StyleWriter *styler;

	SA::Position endPos;
	SA::Position endDoc;

	SA::Position currentPos;
	bool atLineStart;
	bool atLineEnd;
	int state;

	char cursor[3][8];
	SA::Position cursorPos;
	int codePage;
	SA::Position lenCurrent;
	SA::Position lenNext;

	static StylingContext *Context(lua_State *L) noexcept {
		return static_cast<StylingContext *>(
			       lua_touserdata(L, lua_upvalueindex(1)));
	}

	void Colourize() {
		SA::Position end = currentPos - 1;
		if (end >= endDoc)
			end = endDoc - 1;
		styler->ColourTo(end, state);
	}

	static int Line(lua_State *L) {
		StylingContext *context = Context(L);
		const SA::Position position = luaL_checkinteger(L, 2);
		lua_pushinteger(L, context->styler->GetLine(position));
		return 1;
	}

	static int CharAt(lua_State *L) {
		StylingContext *context = Context(L);
		const SA::Position position = luaL_checkinteger(L, 2);
		lua_pushinteger(L, context->styler->SafeGetCharAt(position));
		return 1;
	}

	static int StyleAt(lua_State *L) {
		StylingContext *context = Context(L);
		const SA::Position position = luaL_checkinteger(L, 2);
		lua_pushinteger(L, context->styler->StyleAt(position));
		return 1;
	}

	static int LevelAt(lua_State *L) {
		StylingContext *context = Context(L);
		const SA::Line line = luaL_checkinteger(L, 2);
		lua_pushinteger(L, static_cast<int>(context->styler->LevelAt(line)));
		return 1;
	}

	static int SetLevelAt(lua_State *L) {
		StylingContext *context = Context(L);
		const SA::Line line = luaL_checkinteger(L, 2);
		const int level = luaL_checkint(L, 3);
		context->styler->SetLevel(line, static_cast<SA::FoldLevel>(level));
		return 0;
	}

	static int LineState(lua_State *L) {
		StylingContext *context = Context(L);
		const SA::Line line = luaL_checkinteger(L, 2);
		lua_pushinteger(L, context->styler->GetLineState(line));
		return 1;
	}

	static int SetLineState(lua_State *L) {
		StylingContext *context = Context(L);
		const SA::Line line = luaL_checkinteger(L, 2);
		const int stateOfLine = luaL_checkint(L, 3);
		context->styler->SetLineState(line, stateOfLine);
		return 0;
	}


	void GetNextChar() {
		lenCurrent = lenNext;
		lenNext = 1;
		const SA::Position nextPos = currentPos + lenCurrent;
		unsigned char byteNext = styler->SafeGetCharAt(nextPos);
		size_t nextSlot = (cursorPos + 1) % 3;
		memcpy(cursor[nextSlot], "\0\0\0\0\0\0\0\0", 8);
		cursor[nextSlot][0] = byteNext;
		if (codePage) {
			if (codePage == SA::CpUtf8) {
				if (byteNext >= 0x80) {
					cursor[nextSlot][1] = styler->SafeGetCharAt(nextPos+1);
					lenNext = 2;
					if (byteNext >= 0x80 + 0x40 + 0x20) {
						lenNext = 3;
						cursor[nextSlot][2] = styler->SafeGetCharAt(nextPos+2);
						if (byteNext >= 0x80 + 0x40 + 0x20 + 0x10) {
							lenNext = 4;
							cursor[nextSlot][3] = styler->SafeGetCharAt(nextPos+3);
						}
					}
				}
			} else {
				if (styler->IsLeadByte(byteNext)) {
					lenNext = 2;
					cursor[nextSlot][1] = styler->SafeGetCharAt(nextPos+1);
				}
			}
		}

		// End of line?
		// Trigger on CR only (Mac style) or either on LF from CR+LF (Dos/Win)
		// or on LF alone (Unix). Avoid triggering two times on Dos/Win.
		const char ch = cursor[(cursorPos) % 3][0];
		atLineEnd = (ch == '\r' && cursor[nextSlot][0] != '\n') ||
			    (ch == '\n') ||
			    (currentPos >= endPos);
	}

	void StartStyling(SA::Position startPos_, SA::Position length, int initStyle_) {
		endDoc = styler->Length();
		endPos = startPos_ + length;
		if (endPos == endDoc)
			endPos = endDoc + 1;
		currentPos = startPos_;
		atLineStart = true;
		atLineEnd = false;
		state = initStyle_;
		cursorPos = 0;
		lenCurrent = 0;
		lenNext = 0;
		memcpy(cursor[0], "\0\0\0\0\0\0\0\0", 8);
		memcpy(cursor[1], "\0\0\0\0\0\0\0\0", 8);
		memcpy(cursor[2], "\0\0\0\0\0\0\0\0", 8);
		styler->StartAt(startPos_);
		styler->StartSegment(startPos_);

		GetNextChar();
		cursorPos++;
		GetNextChar();
	}

	static int EndStyling(lua_State *L) {
		StylingContext *context = Context(L);
		context->Colourize();
		return 0;
	}

	static int StartStyling(lua_State *L) {
		StylingContext *context = Context(L);
		const SA::Position startPosStyle = luaL_checkinteger(L, 2);
		const SA::Position lengthStyle = luaL_checkinteger(L, 3);
		const int initialStyle = luaL_checkint(L, 4);
		context->StartStyling(startPosStyle, lengthStyle, initialStyle);
		return 0;
	}

	static int More(lua_State *L) {
		const StylingContext *context = Context(L);
		lua_pushboolean(L, context->currentPos < context->endPos);
		return 1;
	}

	void Forward() {
		if (currentPos < endPos) {
			atLineStart = atLineEnd;
			currentPos += lenCurrent;
			cursorPos++;
			GetNextChar();
		} else {
			atLineStart = false;
			memcpy(cursor[0], "\0\0\0\0\0\0\0\0", 8);
			memcpy(cursor[1], "\0\0\0\0\0\0\0\0", 8);
			memcpy(cursor[2], "\0\0\0\0\0\0\0\0", 8);
			atLineEnd = true;
		}
	}

	static int Forward(lua_State *L) {
		StylingContext *context = Context(L);
		context->Forward();
		return 0;
	}

	static int Position(lua_State *L) {
		const StylingContext *context = Context(L);
		lua_pushinteger(L, context->currentPos);
		return 1;
	}

	static int AtLineStart(lua_State *L) {
		const StylingContext *context = Context(L);
		lua_pushboolean(L, context->atLineStart);
		return 1;
	}

	static int AtLineEnd(lua_State *L) {
		const StylingContext *context = Context(L);
		lua_pushboolean(L, context->atLineEnd);
		return 1;
	}

	static int State(lua_State *L) {
		const StylingContext *context = Context(L);
		lua_pushinteger(L, context->state);
		return 1;
	}

	static int SetState(lua_State *L) {
		StylingContext *context = Context(L);
		context->Colourize();
		context->state = luaL_checkint(L, 2);
		return 0;
	}

	static int ForwardSetState(lua_State *L) {
		StylingContext *context = Context(L);
		context->Forward();
		context->Colourize();
		context->state = luaL_checkint(L, 2);
		return 0;
	}

	static int ChangeState(lua_State *L) {
		StylingContext *context = Context(L);
		context->state = luaL_checkint(L, 2);
		return 0;
	}

	static int Current(lua_State *L) {
		const StylingContext *context = Context(L);
		lua_pushstring(L, context->cursor[context->cursorPos % 3]);
		return 1;
	}

	static int Next(lua_State *L) {
		const StylingContext *context = Context(L);
		lua_pushstring(L, context->cursor[(context->cursorPos + 1) % 3]);
		return 1;
	}

	static int Previous(lua_State *L) {
		const StylingContext *context = Context(L);
		lua_pushstring(L, context->cursor[(context->cursorPos + 2) % 3]);
		return 1;
	}

	static int Token(lua_State *L) {
		StylingContext *context = Context(L);
		const SA::Position start = context->styler->GetStartSegment();
		const SA::Position end = context->currentPos - 1;
		SA::Position len = end - start + 1;
		if (len <= 0)
			len = 1;
		std::string sReturn(len, '\0');
		for (SA::Position i = 0; i < len; i++) {
			sReturn[i] = context->styler->SafeGetCharAt(start + i);
		}
		push_string(L, sReturn);
		return 1;
	}

	bool Match(const char *s) {
		for (SA::Position n=0; *s; n++) {
			if (*s != styler->SafeGetCharAt(currentPos+n))
				return false;
			s++;
		}
		return true;
	}

	static int Match(lua_State *L) {
		StylingContext *context = Context(L);
		const char *s = luaL_checkstring(L, 2);
		lua_pushboolean(L, context->Match(s));
		return 1;
	}

	void PushMethod(lua_State *L, lua_CFunction fn, const char *name) noexcept {
		lua_pushlightuserdata(L, this);
		lua_pushcclosure(L, fn, 1);
		lua_setfield(luaState, -2, name);
	}
};

bool LuaExtension::OnStyle(SA::Position startPos, SA::Position lengthDoc, int initStyle, StyleWriter *styler) {
	bool handled = false;
	if (luaState) {
		if (lua_getglobal(luaState, "OnStyle") != LUA_TNIL) {

			StylingContext sc {};
			sc.startPos = startPos;
			sc.lengthDoc = lengthDoc;
			sc.initStyle = initStyle;
			sc.styler = styler;
			sc.codePage = host->PaneCaller(ExtensionAPI::paneEditor).CodePage();

			lua_newtable(luaState);

			lua_pushstring(luaState, "startPos");
			lua_pushinteger(luaState, startPos);
			lua_settable(luaState, -3);

			lua_pushstring(luaState, "lengthDoc");
			lua_pushinteger(luaState, lengthDoc);
			lua_settable(luaState, -3);

			lua_pushstring(luaState, "initStyle");
			lua_pushinteger(luaState, initStyle);
			lua_settable(luaState, -3);

			lua_pushstring(luaState, "language");
			std::string lang = host->Property("Language");
			push_string(luaState, lang);
			lua_settable(luaState, -3);

			sc.PushMethod(luaState, StylingContext::Line, "Line");
			sc.PushMethod(luaState, StylingContext::CharAt, "CharAt");
			sc.PushMethod(luaState, StylingContext::StyleAt, "StyleAt");
			sc.PushMethod(luaState, StylingContext::LevelAt, "LevelAt");
			sc.PushMethod(luaState, StylingContext::SetLevelAt, "SetLevelAt");
			sc.PushMethod(luaState, StylingContext::LineState, "LineState");
			sc.PushMethod(luaState, StylingContext::SetLineState, "SetLineState");

			sc.PushMethod(luaState, StylingContext::StartStyling, "StartStyling");
			sc.PushMethod(luaState, StylingContext::EndStyling, "EndStyling");
			sc.PushMethod(luaState, StylingContext::More, "More");
			sc.PushMethod(luaState, StylingContext::Forward, "Forward");
			sc.PushMethod(luaState, StylingContext::Position, "Position");
			sc.PushMethod(luaState, StylingContext::AtLineStart, "AtLineStart");
			sc.PushMethod(luaState, StylingContext::AtLineEnd, "AtLineEnd");
			sc.PushMethod(luaState, StylingContext::State, "State");
			sc.PushMethod(luaState, StylingContext::SetState, "SetState");
			sc.PushMethod(luaState, StylingContext::ForwardSetState, "ForwardSetState");
			sc.PushMethod(luaState, StylingContext::ChangeState, "ChangeState");
			sc.PushMethod(luaState, StylingContext::Current, "Current");
			sc.PushMethod(luaState, StylingContext::Next, "Next");
			sc.PushMethod(luaState, StylingContext::Previous, "Previous");
			sc.PushMethod(luaState, StylingContext::Token, "Token");
			sc.PushMethod(luaState, StylingContext::Match, "Match");

			handled = call_function(luaState, 1);
		} else {
			lua_pop(luaState, 1);
		}
	}
	return handled;
}

bool LuaExtension::OnDoubleClick() {
	return CallNamedFunction("OnDoubleClick");
}

bool LuaExtension::OnUpdateUI() {
	return CallNamedFunction("OnUpdateUI");
}

bool LuaExtension::OnMarginClick() {
	return CallNamedFunction("OnMarginClick");
}

bool LuaExtension::OnUserListSelection(int listType, const char *selection) {
	return CallNamedFunction("OnUserListSelection", listType, selection);
}

namespace {

constexpr bool CheckModifiers(int modifiers, SA::KeyMod mod) noexcept {
	return (static_cast<int>(mod) & modifiers) != 0;
}

}

bool LuaExtension::OnKey(int keyval, int modifiers) {
	bool handled = false;
	if (luaState) {
		if (lua_getglobal(luaState, "OnKey") != LUA_TNIL) {
			lua_pushinteger(luaState, keyval);
			lua_pushboolean(luaState, CheckModifiers(modifiers, SA::KeyMod::Shift)); // shift/lock
			lua_pushboolean(luaState, CheckModifiers(modifiers, SA::KeyMod::Ctrl)); // control
			lua_pushboolean(luaState, CheckModifiers(modifiers, SA::KeyMod::Alt)); // alt
			handled = call_function(luaState, 4);
		} else {
			lua_pop(luaState, 1);
		}
	}
	return handled;
}

bool LuaExtension::OnDwellStart(SA::Position pos, const char *word) {
	return CallNamedFunction("OnDwellStart", pos, word);
}

bool LuaExtension::OnClose(const char *filename) {
	return CallNamedFunction("OnClose", filename);
}

bool LuaExtension::OnUserStrip(int control, int change) {
	return CallNamedFunction("OnStrip", control, change);
}

bool LuaExtension::NeedsOnClose() {
	return HasNamedFunction("OnClose");
}
