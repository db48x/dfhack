﻿/*
https://github.com/peterix/dfhack
Copyright (c) 2009-2011 Petr Mrázek (peterix@gmail.com)

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any
damages arising from the use of this software.

Permission is granted to anyone to use this software for any
purpose, including commercial applications, and to alter it and
redistribute it freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must
not claim that you wrote the original software. If you use this
software in a product, an acknowledgment in the product documentation
would be appreciated but is not required.

2. Altered source versions must be plainly marked as such, and
must not be misrepresented as being the original software.

3. This notice may not be removed or altered from any source
distribution.
*/

#include "Internal.h"

#include <string>
#include <vector>
#include <map>

#include "MemAccess.h"
#include "Core.h"
#include "VersionInfo.h"
#include "tinythread.h"
// must be last due to MS stupidity
#include "DataDefs.h"
#include "DataIdentity.h"
#include "LuaWrapper.h"

#include "MiscUtils.h"

#include <lua.h>
#include <lauxlib.h>

using namespace DFHack;
using namespace DFHack::LuaWrapper;

static luaL_Reg no_functions[] = { { NULL, NULL } };

/**
 * Report an error while accessing a field (index = field name).
 */
void LuaWrapper::field_error(lua_State *state, int index, const char *err, const char *mode)
{
    lua_getfield(state, UPVAL_METATABLE, "__metatable");
    const char *cname = lua_tostring(state, -1);
    const char *fname = index ? lua_tostring(state, index) : "*";
    luaL_error(state, "Cannot %s field %s.%s: %s.",
               mode, (cname ? cname : "?"), (fname ? fname : "?"), err);
}

void DFHack::PushDFObject(lua_State *state, type_identity *type, void *ptr)
{
    push_object_internal(state, type, ptr, false);
}

void *DFHack::GetDFObject(lua_State *state, type_identity *type, int val_index, bool exact_type)
{
    return get_object_internal(state, type, val_index, exact_type, false);
}

/* */

static int change_error(lua_State *state)
{
    luaL_error(state, "Attempt to change a read-only table.\n");
    return 0;
}

/**
 * Wrap a table so that it can't be modified.
 */
static void freeze_table(lua_State *state, bool leave_metatable = false, const char *name = NULL)
{
    // rv = {}; setmetatable(rv, { __index = in, __newindex = change_error, __metatable = name })
    int base = lua_gettop(state);
    lua_newtable(state);
    lua_swap(state);
    lua_setfield(state, base, "__index");
    lua_getfield(state, LUA_REGISTRYINDEX, DFHACK_CHANGEERROR_NAME);
    lua_setfield(state, base, "__newindex");
    lua_newtable(state);
    lua_swap(state);
    lua_dup(state);
    lua_setmetatable(state, base);
    if (name)
    {
        lua_pushstring(state, name);
        lua_setfield(state, -2, "__metatable");
    }
    // result: [frozen table] [metatable]
    if (!leave_metatable)
        lua_pop(state, 1);
}

static void LookupInTable(lua_State *state, const char *tname)
{
    lua_getfield(state, LUA_REGISTRYINDEX, tname);
    lua_swap(state);
    lua_rawget(state, -2);
    lua_remove(state, -2);
}

/**
 * Look up the key on the stack in DFHACK_TYPETABLE;
 * if found, put result on the stack and return true.
 */
bool LuaWrapper::LookupTypeInfo(lua_State *state, bool in_method)
{
    // stack: [lookup key]

    if (in_method)
    {
        lua_rawget(state, UPVAL_TYPETABLE);
    }
    else
    {
        LookupInTable(state, DFHACK_TYPETABLE_NAME);
    }

    // stack: [info]

    if (!lua_islightuserdata(state, -1))
    {
        lua_pop(state, 1);
        return false;
    }
    else
        return true;
}

void LuaWrapper::LookupInTable(lua_State *state, void *id, const char *tname)
{
    lua_getfield(state, LUA_REGISTRYINDEX, tname);
    lua_pushlightuserdata(state, id);
    lua_rawget(state, -2);
    lua_remove(state, -2);
}

void LuaWrapper::SaveInTable(lua_State *state, void *node, const char *tname)
{
    // stack: [info]
    lua_getfield(state, LUA_REGISTRYINDEX, tname);

    lua_pushlightuserdata(state, node);
    lua_pushvalue(state, -3);
    lua_rawset(state, -3);

    lua_pushvalue(state, -2);
    lua_pushlightuserdata(state, node);
    lua_rawset(state, -3);

    lua_pop(state, 1);
    // stack: [info]
}

void LuaWrapper::SaveTypeInfo(lua_State *state, void *node)
{
    SaveInTable(state, node, DFHACK_TYPETABLE_NAME);
}

static void BuildTypeMetatable(lua_State *state, type_identity *type);

/**
 * Push the pointer as DF object ref using metatable on the stack.
 */
void LuaWrapper::push_object_ref(lua_State *state, void *ptr)
{
    // stack: [metatable]
    auto ref = (DFRefHeader*)lua_newuserdata(state, sizeof(DFRefHeader));
    ref->ptr = ptr;

    lua_swap(state);
    lua_setmetatable(state, -2);
    // stack: [userdata]
}

void *LuaWrapper::get_object_ref(lua_State *state, int val_index)
{
    assert(!lua_islightuserdata(state, val_index));

    auto ref = (DFRefHeader*)lua_touserdata(state, val_index);
    return ref->ptr;
}

/**
 * Push the pointer using given identity.
 */
void LuaWrapper::push_object_internal(lua_State *state, type_identity *type, void *ptr, bool in_method)
{
    /*
     * If NULL pointer or no type, push something simple
     */

    if (!ptr || !type)
    {
        if (!ptr)
            lua_pushnil(state);
        else
            lua_pushlightuserdata(state, ptr);

        return;
    }

    /*
     * Resolve actual class using vtable
     */

    if (type->type() == IDTYPE_CLASS)
    {
        virtual_identity *class_vid = virtual_identity::get(virtual_ptr(ptr));
        if (class_vid)
            type = class_vid;
    }

    /*
     * Resolve metatable by identity, and push the object
     */

    lua_pushlightuserdata(state, type); // () -> type

    if (!LookupTypeInfo(state, in_method)) // type -> metatable?
        BuildTypeMetatable(state, type); // () -> metatable

    push_object_ref(state, ptr); // metatable -> userdata
}

static void fetch_container_details(lua_State *state, int meta, type_identity **pitem, int *pcount)
{
    if (!meta) return;

    lua_getfield(state, meta, "_field_identity");
    *pitem = (type_identity*)lua_touserdata(state, -1);
    lua_pop(state, 1);

    if (pcount)
    {
        lua_getfield(state, meta, "_count");
        if (lua_isnumber(state, -1))
            *pcount = lua_tointeger(state, -1);
        lua_pop(state, 1);
    }
}

/**
 * Check if type1 and type2 are compatible, possibly using additional metatable data.
 */
bool LuaWrapper::is_type_compatible(lua_State *state, type_identity *type1, int meta1,
                                    type_identity *type2, int meta2, bool exact_equal)
{
    if (type1 == type2)
        return true;
    if (!exact_equal && !type1)
        return true;
    if (!type1 || !type2)
        return false;

    auto t1 = type1->type();
    if (t1 != type2->type())
        return false;

    switch (t1)
    {
    case IDTYPE_POINTER:
        return is_type_compatible(state,
                                  ((pointer_identity*)type1)->getTarget(), 0,
                                  ((pointer_identity*)type2)->getTarget(), 0,
                                  exact_equal);
        break;

    case IDTYPE_BUFFER:
    {
        auto b1 = (df::buffer_container_identity*)type1;
        auto b2 = (df::buffer_container_identity*)type2;
        type_identity *item1 = b1->getItemType(), *item2 = b2->getItemType();
        int count1 = b1->getSize(), count2 = b2->getSize();

        fetch_container_details(state, meta1, &item1, &count1);
        fetch_container_details(state, meta2, &item2, &count2);

        return item1 && item2 && count1 == count2 &&
               is_type_compatible(state, item1, 0, item2, 0, true);
    }

    case IDTYPE_STL_PTR_VECTOR:
    {
        auto b1 = (df::stl_ptr_vector_identity*)type1;
        auto b2 = (df::stl_ptr_vector_identity*)type2;
        type_identity *item1 = b1->getItemType(), *item2 = b2->getItemType();

        fetch_container_details(state, meta1, &item1, NULL);
        fetch_container_details(state, meta1, &item2, NULL);

        return is_type_compatible(state, item1, 0, item2, 0, exact_equal);
    }

    case IDTYPE_STRUCT:
    case IDTYPE_CLASS:
    {
        auto b1 = (struct_identity*)type1;
        auto b2 = (struct_identity*)type2;

        return (!exact_equal && b1->is_subclass(b2));
    }

    default:
        return false;
    }
}

static bool is_type_compatible(lua_State *state, type_identity *type1, int meta1,
                               int meta2, bool exact_equal)
{
    lua_getfield(state, meta2, "_identity");
    auto type2 = (type_identity*)lua_touserdata(state, -1);
    lua_pop(state, 1);

    return is_type_compatible(state, type1, meta1, type2, meta2, exact_equal);
}

static bool is_type_compatible(lua_State *state, int meta1, int meta2, bool exact_equal)
{
    if (lua_rawequal(state, meta1, meta2))
        return true;

    lua_getfield(state, meta1, "_identity");
    auto type1 = (type_identity*)lua_touserdata(state, -1);
    lua_pop(state, 1);

    return is_type_compatible(state, type1, meta1, meta2, exact_equal);
}

/**
 * Verify that the value matches the identity, and return ptr if so.
 */
void *LuaWrapper::get_object_internal(lua_State *state, type_identity *type, int val_index, bool exact_type, bool in_method)
{
    /*
     * Non-userdata results in NULL; nil for NULL gets handled here too.
     */
    if (!lua_isuserdata(state, val_index))
        return NULL;

    /*
     * Light user data is allowed with null type; otherwise bail out.
     */
    if (!lua_getmetatable(state, val_index)) // () -> metatable?
    {
        if (!type && lua_islightuserdata(state, val_index))
            return lua_touserdata(state, val_index);

        return NULL;
    }

    /*
     * Verify that the metatable is known, and refers to the correct type.
     * Here doing reverse lookup of identity by metatable.
     */
    if (!LookupTypeInfo(state, in_method)) // metatable -> type?
        return NULL;

    if (type && lua_touserdata(state, -1) != type)
    {
        /*
         * If valid but different type, do an intelligent comparison.
         */
        lua_pop(state, 1); // type -> ()
        lua_getmetatable(state, val_index);

        if (!is_type_compatible(state, type, 0, lua_gettop(state), exact_type))
        {
            lua_pop(state, 1); // metatable -> ()
            return NULL;
        }
    }

    lua_pop(state, 1); // type -> ()

    /*
     * Finally decode the reference.
     */
    return get_object_ref(state, val_index);
}

/**
 * Check if the object and metatable are a valid DF reference or type.
 */
static bool is_valid_metatable(lua_State *state, int objidx, int metaidx)
{
    // Verify object type validity
    if (lua_isuserdata(state, objidx))
    {
        lua_pushvalue(state, metaidx);
        lua_rawget(state, UPVAL_TYPETABLE);
    }
    else
    {
        lua_pushvalue(state, objidx);
        LookupInTable(state, DFHACK_TYPEID_TABLE_NAME);
    }

    bool ok = !lua_isnil(state, -1);
    lua_pop(state, 1);
    return ok;
}

/**
 * Given a DF object reference or type, safely retrieve its identity pointer.
 */
type_identity *LuaWrapper::get_object_identity(lua_State *state, int objidx,
                                               const char *ctx, bool allow_type,
                                               bool keep_metatable)
{
    if (!lua_getmetatable(state, objidx))
        luaL_error(state, "Invalid object in %s", ctx);

    if (!allow_type && !lua_isuserdata(state, objidx))
        luaL_error(state, "Object expected in %s", ctx);

    if (!is_valid_metatable(state, objidx, -1))
        luaL_error(state, "Invalid object metatable in %s", ctx);

    // Extract identity from metatable
    lua_getfield(state, -1, "_identity");

    type_identity *id = (type_identity*)lua_touserdata(state, -1);
    if (!id)
        luaL_error(state, "Invalid object identity in %s", ctx);

    lua_pop(state, keep_metatable ? 1 : 2);
    return id;
}

static void check_type_compatible(lua_State *state, int obj1, int obj2,
                                  type_identity **type1, type_identity **type2,
                                  const char *ctx, bool allow_type, bool exact)
{
    int base = lua_gettop(state);

    *type1 = get_object_identity(state, obj1, ctx, allow_type, true);
    *type2 = get_object_identity(state, obj2, ctx, allow_type, true);

    if (!is_type_compatible(state, *type1, base+1, *type2, base+2, exact))
    {
        lua_getfield(state, base+1, "__metatable");
        const char *cname1 = lua_tostring(state, -1);
        lua_getfield(state, base+2, "__metatable");
        const char *cname2 = lua_tostring(state, -1);

        luaL_error(state, "Types %s and %s incompatible in %s", cname1, cname2, ctx);
    }

    lua_pop(state, 2);
}

/**
 * Metamethod: compare two DF object references.
 *
 * Equal if same pointer and same metatable.
 */
static int meta_ptr_compare(lua_State *state)
{
    if (!lua_isuserdata(state, 1) || !lua_isuserdata(state, 2) ||
        !lua_getmetatable(state, 1) || !lua_getmetatable(state, 2) ||
        get_object_ref(state, 1) != get_object_ref(state, 2) ||
        !is_type_compatible(state, 3, 4, true))
    {
        lua_pushboolean(state, false);
        return 1;
    }

    lua_pushboolean(state, true);
    return 1;
}

/**
 * Method: sizeof for DF object references.
 *
 * Returns: size[, address]
 */
static int meta_sizeof(lua_State *state)
{
    int argc = lua_gettop(state);

    if (argc != 1)
        luaL_error(state, "Usage: object:sizeof() or df.sizeof(object)");

    // Two special cases: nil and lightuserdata for NULL and void*
    if (lua_isnil(state, 1) || lua_islightuserdata(state, 1))
    {
        lua_pushnil(state);
        lua_pushnumber(state, (size_t)lua_touserdata(state, 1));
        return 2;
    }

    type_identity *id = get_object_identity(state, 1, "df.sizeof()", true);

    lua_pushinteger(state, id->byte_size());

    if (lua_isuserdata(state, 1))
    {
        lua_pushnumber(state, (size_t)get_object_ref(state, 1));
        return 2;
    }
    else
        return 1;
}

/**
 * Method: displace for DF object references.
 *
 * Returns: a reference with the same type, but modified address
 */
static int meta_displace(lua_State *state)
{
    int argc = lua_gettop(state);

    bool has_step = (argc >= 3);
    if ((argc < 2 || argc > 3) ||
        !lua_isnumber(state, 2) ||
        (has_step && !lua_isnumber(state, 3)))
    {
        luaL_error(state, "Usage: object:_displace(index[,step]) or df._displace(object,...)");
    }

    int index = lua_tointeger(state, 2);
    int step = has_step ? lua_tointeger(state, 3) : 1;

    // Two special cases: nil and lightuserdata for NULL and void*
    if (lua_isnil(state, 1))
    {
        lua_pushnil(state);
        return 1;
    }

    if (lua_islightuserdata(state, 1))
    {
        if (!has_step)
            luaL_error(state, "Step is mandatory in _displace of void*");

        auto ptr = (uint8_t*)lua_touserdata(state, 1);
        lua_pushlightuserdata(state, ptr + index*step);
        return 1;
    }

    type_identity *id = get_object_identity(state, 1, "df._displace()");

    if (!has_step)
        step = id->byte_size();

    if (index == 0 || step == 0)
    {
        lua_pushvalue(state, 1);
    }
    else
    {
        auto ptr = (uint8_t*)get_object_ref(state, 1);
        lua_getmetatable(state, 1);
        push_object_ref(state, ptr + index*step);
    }

    return 1;
}

/**
 * Method: allocation for DF object references.
 */
static int meta_new(lua_State *state)
{
    int argc = lua_gettop(state);

    if (argc != 1)
        luaL_error(state, "Usage: object:new() or df.new(object)");

    type_identity *id = get_object_identity(state, 1, "df.new()", true);

    void *ptr = id->allocate();
    if (!ptr)
        luaL_error(state, "Cannot allocate %s", id->getFullName().c_str());

    if (lua_isuserdata(state, 1))
    {
        lua_getmetatable(state, 1);
        push_object_ref(state, ptr);

        id->copy(ptr, get_object_ref(state, 1));
    }
    else
        push_object_internal(state, id, ptr);

    return 1;
}

static void invoke_resize(lua_State *state, int table, lua_Integer size)
{
    lua_getfield(state, table, "resize");
    lua_pushvalue(state, table);
    lua_pushinteger(state, size);
    lua_call(state, 2, 0);
}

static void copy_table(lua_State *state, int dest, int src, int skipbase)
{
    // stack: (skipbase) skipkey skipkey |

    int top = lua_gettop(state);

    lua_pushnil(state);

    while (lua_next(state, src))
    {
        for (int i = skipbase+1; i <= top; i++)
        {
            if (lua_rawequal(state, -2, i))
            {
                lua_pop(state, 1);
                goto next_outer;
            }
        }

        {
            lua_pushvalue(state, -2);
            lua_swap(state);
            lua_settable(state, dest);
        }

    next_outer:;
    }
}

/**
 * Method: assign data between objects.
 */
static int meta_assign(lua_State *state)
{
    int argc = lua_gettop(state);

    if (argc != 2)
        luaL_error(state, "Usage: target:assign(src) or df.assign(target,src)");

    if (!lua_istable(state, 2))
    {
        type_identity *id1, *id2;
        check_type_compatible(state, 1, 2, &id1, &id2, "df.assign()", false, false);

        if (!id1->copy(get_object_ref(state, 1), get_object_ref(state, 2)))
            luaL_error(state, "No copy support for %s", id1->getFullName().c_str());
    }
    else
    {
        type_identity *id = get_object_identity(state, 1, "df.assign()", false);

        int base = lua_gettop(state);

        // x:assign{ assign = foo } => x:assign(foo)
        bool has_assign = false;

        lua_pushstring(state, "assign");
        lua_dup(state);
        lua_rawget(state, 2);

        if (!lua_isnil(state,-1))
        {
            has_assign = true;
            lua_getfield(state, LUA_REGISTRYINDEX, DFHACK_ASSIGN_NAME);
            lua_pushvalue(state, 1);
            lua_pushvalue(state, base+2);
            lua_call(state, 2, 0);
        }

        lua_pop(state, 1);

        // new is used by autovivification and should be skipped
        lua_pushstring(state, "new");

        if (id->isContainer())
        {
            // check resize field
            lua_pushstring(state, "resize");
            lua_dup(state);
            lua_rawget(state, 2);

            if (lua_isnil(state,-1) && !has_assign)
            {
                /*
                 * no assign && nil or missing resize field => 1-based lua array
                 */
                int size = lua_objlen(state, 2);

                lua_pop(state, 1);
                invoke_resize(state, 1, size);

                for (int i = 1; i <= size; i++)
                {
                    lua_pushinteger(state, i-1);
                    lua_rawgeti(state, 2, i);
                    lua_settable(state, 1);
                }
            }
            else
            {
                if (lua_isboolean(state, -1) || lua_isnil(state, -1))
                {
                    // resize=false => just assign
                    // resize=true => find the largest index
                    if (lua_toboolean(state, -1))
                    {
                        lua_Integer size = 0;

                        lua_pushnil(state);
                        while (lua_next(state, 2))
                        {
                            lua_pop(state, 1);
                            if (lua_isnumber(state,-1))
                                size = std::max(size, lua_tointeger(state,-1)+1);
                        }

                        invoke_resize(state, 1, size);
                    }
                }
                else
                {
                    // otherwise, must be an explicit number
                    if (!lua_isnumber(state,-1))
                        luaL_error(state, "Invalid container.resize value in df.assign()");

                    invoke_resize(state, 1, lua_tointeger(state, -1));
                }

                lua_pop(state, 1);
                copy_table(state, 1, 2, base);
            }
        }
        else
        {

            copy_table(state, 1, 2, base);
        }
    }

    return 0;
}

/**
 * Method: deallocation for DF object references.
 */
static int meta_delete(lua_State *state)
{
    int argc = lua_gettop(state);

    if (argc != 1)
        luaL_error(state, "Usage: object:delete() or df.delete(object)");

    if (lua_isnil(state, 1))
    {
        lua_pushboolean(state, true);
        return 1;
    }

    type_identity *id = get_object_identity(state, 1, "df.delete()", false);

    bool ok = id->destroy(get_object_ref(state, 1));

    lua_pushboolean(state, ok);
    return 1;
}

/**
 * Verify that the object is a DF ref with UPVAL_METATABLE.
 * If everything ok, extract the address.
 */
uint8_t *LuaWrapper::get_object_addr(lua_State *state, int obj, int field, const char *mode)
{
    if (!lua_isuserdata(state, obj) ||
        !lua_getmetatable(state, obj))
        field_error(state, field, "invalid object", mode);

    if (!lua_rawequal(state, -1, UPVAL_METATABLE))
        field_error(state, field, "invalid object metatable", mode);

    lua_pop(state, 1);

    return (uint8_t*)get_object_ref(state, obj);
}

/**
 * Metamethod: represent a type node as string.
 */
static int meta_type_tostring(lua_State *state)
{
    if (!lua_getmetatable(state, 1))
        return 0;

    lua_getfield(state, -1, "__metatable");
    const char *cname = lua_tostring(state, -1);

    lua_pushstring(state, stl_sprintf("<type: %s>", cname).c_str());
    return 1;
}

/**
 * Metamethod: represent a DF object reference as string.
 */
static int meta_ptr_tostring(lua_State *state)
{
    uint8_t *ptr = get_object_addr(state, 1, 0, "access");

    lua_getfield(state, UPVAL_METATABLE, "__metatable");
    const char *cname = lua_tostring(state, -1);

    lua_pushstring(state, stl_sprintf("<%s: 0x%08x>", cname, (unsigned)ptr).c_str());
    return 1;
}

/**
 * Metamethod: __index for enum.attrs
 */
static int meta_enum_attr_index(lua_State *state)
{
    if (!lua_isnumber(state, 2))
        lua_rawget(state, UPVAL_FIELDTABLE);
    if (!lua_isnumber(state, 2))
        luaL_error(state, "Invalid index in enum.attrs[]");

    auto id = (enum_identity*)lua_touserdata(state, lua_upvalueindex(2));

    int64_t idx = lua_tonumber(state, 2);
    if (idx < id->getFirstItem() || idx > id->getLastItem())
        idx = id->getLastItem()+1;
    idx -= id->getFirstItem();

    uint8_t *ptr = (uint8_t*)id->getAttrs();
    auto atype = id->getAttrType();

    push_object_internal(state, atype, ptr + unsigned(atype->byte_size()*idx));
    return 1;
}

static int meta_nodata(lua_State *state)
{
    return 0;
}

/**
 * Metamethod: __pairs, returning 1st upvalue as iterator
 */
static int meta_pairs(lua_State *state)
{
    luaL_checkany(state, 1);
    lua_pushvalue(state, lua_upvalueindex(1));
    lua_pushvalue(state, 1);
    lua_pushnil(state);
    return 3;
}

/**
 * Make a metatable with most common fields, and an empty table for UPVAL_FIELDTABLE.
 */
void LuaWrapper::MakeMetatable(lua_State *state, type_identity *type, const char *kind)
{
    int base = lua_gettop(state);
    lua_newtable(state); // metatable

    lua_pushstring(state, type->getFullName().c_str());
    lua_setfield(state, base+1, "__metatable");

    lua_pushlightuserdata(state, type);
    lua_setfield(state, base+1, "_identity");

    LookupInTable(state, type, DFHACK_TYPEID_TABLE_NAME);
    if (lua_isnil(state, -1))
    {
        // Copy the string from __metatable if no real type
        lua_pop(state, 1);
        lua_getfield(state, base+1, "__metatable");
    }
    lua_setfield(state, base+1, "_type");

    lua_pushstring(state, kind);
    lua_setfield(state, base+1, "_kind");

    // Create the field table
    lua_newtable(state);
}

/**
 * Enable a metafield by injecting an entry into a UPVAL_FIELDTABLE.
 */
void LuaWrapper::EnableMetaField(lua_State *state, int ftable_idx, const char *name, void *id)
{
    lua_pushlightuserdata(state, id);
    lua_setfield(state, ftable_idx, name);
}

/**
 * Set metatable properties common to all actual DF object references.
 */
void LuaWrapper::SetPtrMethods(lua_State *state, int meta_idx, int read_idx)
{
    lua_getfield(state, LUA_REGISTRYINDEX, DFHACK_COMPARE_NAME);
    lua_setfield(state, meta_idx, "__eq");

    lua_getfield(state, LUA_REGISTRYINDEX, DFHACK_TYPETABLE_NAME);
    lua_pushvalue(state, meta_idx);
    lua_pushcclosure(state, meta_ptr_tostring, 2);
    lua_setfield(state, meta_idx, "__tostring");

    EnableMetaField(state, read_idx, "_type");
    EnableMetaField(state, read_idx, "_kind");

    EnableMetaField(state, read_idx, "_field");

    lua_getfield(state, LUA_REGISTRYINDEX, DFHACK_SIZEOF_NAME);
    lua_setfield(state, meta_idx, "sizeof");
    EnableMetaField(state, read_idx, "sizeof");

    lua_getfield(state, LUA_REGISTRYINDEX, DFHACK_NEW_NAME);
    lua_setfield(state, meta_idx, "new");
    EnableMetaField(state, read_idx, "new");

    lua_getfield(state, LUA_REGISTRYINDEX, DFHACK_DELETE_NAME);
    lua_setfield(state, meta_idx, "delete");
    EnableMetaField(state, read_idx, "delete");

    lua_getfield(state, LUA_REGISTRYINDEX, DFHACK_ASSIGN_NAME);
    lua_setfield(state, meta_idx, "assign");
    EnableMetaField(state, read_idx, "assign");

    lua_getfield(state, LUA_REGISTRYINDEX, DFHACK_DISPLACE_NAME);
    lua_setfield(state, meta_idx, "_displace");
    EnableMetaField(state, read_idx, "_displace");
}

/**
 * Add a __pairs/__ipairs metamethod using iterator on the top of stack.
 */
void LuaWrapper::SetPairsMethod(lua_State *state, int meta_idx, const char *name)
{
    if (lua_isnil(state, -1))
    {
        lua_pop(state, 1);
        lua_pushcfunction(state, meta_nodata);
    }

    lua_pushcclosure(state, meta_pairs, 1);
    lua_setfield(state, meta_idx, name);
}

/**
 * Add a struct-style (3 upvalues) metamethod to the metatable.
 */
void LuaWrapper::PushStructMethod(lua_State *state, int meta_idx, int ftable_idx,
                                  lua_CFunction function)
{
    lua_getfield(state, LUA_REGISTRYINDEX, DFHACK_TYPETABLE_NAME);
    lua_pushvalue(state, meta_idx);
    lua_pushvalue(state, ftable_idx);
    lua_pushcclosure(state, function, 3);
}

/**
 * Add a struct-style (3 upvalues) metamethod to the metatable.
 */
void LuaWrapper::SetStructMethod(lua_State *state, int meta_idx, int ftable_idx,
                                 lua_CFunction function, const char *name)
{
    PushStructMethod(state, meta_idx, ftable_idx, function);
    lua_setfield(state, meta_idx, name);
}

/**
 * Add a 6 upvalue metamethod to the metatable.
 */
void LuaWrapper::PushContainerMethod(lua_State *state, int meta_idx, int ftable_idx,
                                     lua_CFunction function,
                                     type_identity *container, type_identity *item, int count)
{
    lua_getfield(state, LUA_REGISTRYINDEX, DFHACK_TYPETABLE_NAME);
    lua_pushvalue(state, meta_idx);
    lua_pushvalue(state, ftable_idx);

    lua_pushlightuserdata(state, container);
    lua_pushlightuserdata(state, item);
    if (count < 0)
        lua_pushnil(state);
    else
        lua_pushinteger(state, count);

    lua_pushcclosure(state, function, 6);
}

/**
 * Add a 6 upvalue metamethod to the metatable.
 */
void LuaWrapper::SetContainerMethod(lua_State *state, int meta_idx, int ftable_idx,
                                    lua_CFunction function, const char *name,
                                    type_identity *container, type_identity *item, int count)
{
    PushContainerMethod(state, meta_idx, ftable_idx, function, container, item, count);
    lua_setfield(state, meta_idx, name);
}

/**
 * If ienum refers to a valid enum, attach its keys to UPVAL_FIELDTABLE,
 * and the enum itself to the _enum metafield. Pushes the key table on the stack
 */
void LuaWrapper::AttachEnumKeys(lua_State *state, int meta_idx, int ftable_idx, type_identity *ienum)
{
    EnableMetaField(state, ftable_idx, "_enum");

    LookupInTable(state, ienum, DFHACK_TYPEID_TABLE_NAME);
    lua_setfield(state, meta_idx, "_enum");

    LookupInTable(state, ienum, DFHACK_ENUM_TABLE_NAME);

    if (!lua_isnil(state, -1))
    {
        lua_dup(state);
        lua_newtable(state);
        lua_swap(state);
        lua_setfield(state, -2, "__index");
        lua_setmetatable(state, ftable_idx);
    }
    else
    {
        lua_pop(state, 1);
        lua_getfield(state, LUA_REGISTRYINDEX, DFHACK_EMPTY_TABLE_NAME);
    }

    lua_dup(state);
    lua_setfield(state, meta_idx, "_index_table");
}

static void BuildTypeMetatable(lua_State *state, type_identity *type)
{
    type->build_metatable(state);

    lua_pop(state, 1);

    SaveTypeInfo(state, type);
}

/*
 * Recursive walk of scopes to construct the df... tree.
 */

static void RenderTypeChildren(lua_State *state, const std::vector<compound_identity*> &children);

void LuaWrapper::AssociateId(lua_State *state, int table, int val, const char *name)
{
    lua_pushinteger(state, val);
    lua_pushstring(state, name);
    lua_dup(state);
    lua_pushinteger(state, val);

    lua_rawset(state, table);
    lua_rawset(state, table);
}

static void FillEnumKeys(lua_State *state, int ftable, enum_identity *eid)
{
    const char *const *keys = eid->getKeys();

    // Create a new table attached to ftable as __index
    lua_newtable(state);

    lua_dup(state);
    lua_setmetatable(state, ftable);

    int base = lua_gettop(state);

    lua_newtable(state);

    // For enums, set mapping between keys and values
    for (int64_t i = eid->getFirstItem(), j = 0; i <= eid->getLastItem(); i++, j++)
    {
        if (keys[j])
            AssociateId(state, base+1, i, keys[j]);
    }

    if (eid->getFirstItem() <= eid->getLastItem())
    {
        lua_pushinteger(state, eid->getFirstItem());
        lua_setfield(state, base+1, "_first_item");

        lua_pushinteger(state, eid->getLastItem());
        lua_setfield(state, base+1, "_last_item");
    }

    SaveInTable(state, eid, DFHACK_ENUM_TABLE_NAME);

    // Add an attribute table if any
    if (eid->getAttrs())
    {
        lua_getfield(state, LUA_REGISTRYINDEX, DFHACK_TYPETABLE_NAME);
        lua_pushlightuserdata(state, eid);
        lua_pushvalue(state, base+1);
        lua_pushcclosure(state, meta_enum_attr_index, 3);

        freeze_table(state, false, (eid->getFullName()+".attrs").c_str());
        lua_setfield(state, ftable, "attrs");
    }

    lua_setfield(state, base, "__index");
    lua_pop(state, 1);
}

static void FillBitfieldKeys(lua_State *state, int ftable, bitfield_identity *eid)
{
    auto bits = eid->getBits();

    for (int i = 0; i < eid->getNumBits(); i++)
    {
        if (bits[i].name)
            AssociateId(state, ftable, i, bits[i].name);
        if (bits[i].size > 1)
            i += bits[i].size-1;
    }

    lua_pushinteger(state, 0);
    lua_setfield(state, ftable, "_first_item");

    lua_pushinteger(state, eid->getNumBits()-1);
    lua_setfield(state, ftable, "_last_item");

    SaveInTable(state, eid, DFHACK_ENUM_TABLE_NAME);
}

static void RenderType(lua_State *state, compound_identity *node)
{
    assert(node->getName());
    std::string name = node->getFullName();

    int base = lua_gettop(state);

    lua_newtable(state);
    if (!lua_checkstack(state, 20))
        return;

    SaveInTable(state, node, DFHACK_TYPEID_TABLE_NAME);

    // metatable
    lua_newtable(state);

    lua_dup(state);
    lua_setmetatable(state, base+1);

    lua_pushstring(state, name.c_str());
    lua_setfield(state, base+2, "__metatable");

    lua_getfield(state, LUA_REGISTRYINDEX, DFHACK_TYPE_TOSTRING_NAME);
    lua_setfield(state, base+2, "__tostring");

    lua_pushlightuserdata(state, node);
    lua_setfield(state, base+2, "_identity");

    // inner table
    lua_newtable(state);

    lua_dup(state);
    lua_setfield(state, base+2, "__index");

    int ftable = base+3;

    switch (node->type())
    {
    case IDTYPE_STRUCT:
        lua_pushstring(state, "struct-type");
        lua_setfield(state, ftable, "_kind");
        IndexStatics(state, base+2, base+3, (struct_identity*)node);
        break;

    case IDTYPE_CLASS:
        lua_pushstring(state, "class-type");
        lua_setfield(state, ftable, "_kind");
        IndexStatics(state, base+2, base+3, (struct_identity*)node);
        break;

    case IDTYPE_ENUM:
        lua_pushstring(state, "enum-type");
        lua_setfield(state, ftable, "_kind");
        FillEnumKeys(state, ftable, (enum_identity*)node);
        break;

    case IDTYPE_BITFIELD:
        lua_pushstring(state, "bitfield-type");
        lua_setfield(state, ftable, "_kind");
        FillBitfieldKeys(state, ftable, (bitfield_identity*)node);
        break;

    case IDTYPE_GLOBAL:
        {
            RenderTypeChildren(state, node->getScopeChildren());

            BuildTypeMetatable(state, node);

            lua_dup(state);
            lua_setmetatable(state, base+3);

            lua_getfield(state, -1, "__newindex");
            lua_setfield(state, base+2, "__newindex");
            lua_getfield(state, -1, "__pairs");
            lua_setfield(state, base+2, "__pairs");

            lua_pop(state, 3);
            return;
        }

    default:
        break;
    }

    RenderTypeChildren(state, node->getScopeChildren());

    lua_getfield(state, LUA_REGISTRYINDEX, DFHACK_SIZEOF_NAME);
    lua_setfield(state, ftable, "sizeof");

    lua_getfield(state, LUA_REGISTRYINDEX, DFHACK_NEW_NAME);
    lua_setfield(state, ftable, "new");

    lua_pop(state, 2);
}

static void RenderTypeChildren(lua_State *state, const std::vector<compound_identity*> &children)
{
    for (size_t i = 0; i < children.size(); i++)
    {
        RenderType(state, children[i]);
        lua_pushstring(state, children[i]->getName());
        lua_swap(state);
        lua_rawset(state, -3);
    }
}

static void DoAttach(lua_State *state)
{
    int base = lua_gettop(state);

    lua_newtable(state);
    lua_setfield(state, LUA_REGISTRYINDEX, DFHACK_PTR_IDTABLE_NAME);

    lua_newtable(state);
    lua_setfield(state, LUA_REGISTRYINDEX, DFHACK_TYPEID_TABLE_NAME);

    lua_newtable(state);
    lua_setfield(state, LUA_REGISTRYINDEX, DFHACK_ENUM_TABLE_NAME);

    lua_newtable(state);
    lua_setfield(state, LUA_REGISTRYINDEX, DFHACK_EMPTY_TABLE_NAME);

    lua_pushcfunction(state, change_error);
    lua_setfield(state, LUA_REGISTRYINDEX, DFHACK_CHANGEERROR_NAME);

    lua_pushcfunction(state, meta_ptr_compare);
    lua_setfield(state, LUA_REGISTRYINDEX, DFHACK_COMPARE_NAME);

    lua_pushcfunction(state, meta_type_tostring);
    lua_setfield(state, LUA_REGISTRYINDEX, DFHACK_TYPE_TOSTRING_NAME);

    lua_getfield(state, LUA_REGISTRYINDEX, DFHACK_TYPETABLE_NAME);
    lua_pushcclosure(state, meta_sizeof, 1);
    lua_setfield(state, LUA_REGISTRYINDEX, DFHACK_SIZEOF_NAME);

    lua_getfield(state, LUA_REGISTRYINDEX, DFHACK_TYPETABLE_NAME);
    lua_pushcclosure(state, meta_displace, 1);
    lua_setfield(state, LUA_REGISTRYINDEX, DFHACK_DISPLACE_NAME);

    lua_getfield(state, LUA_REGISTRYINDEX, DFHACK_TYPETABLE_NAME);
    lua_pushcclosure(state, meta_new, 1);
    lua_setfield(state, LUA_REGISTRYINDEX, DFHACK_NEW_NAME);

    lua_getfield(state, LUA_REGISTRYINDEX, DFHACK_TYPETABLE_NAME);
    lua_pushcclosure(state, meta_assign, 1);
    lua_setfield(state, LUA_REGISTRYINDEX, DFHACK_ASSIGN_NAME);

    lua_getfield(state, LUA_REGISTRYINDEX, DFHACK_TYPETABLE_NAME);
    lua_pushcclosure(state, meta_delete, 1);
    lua_setfield(state, LUA_REGISTRYINDEX, DFHACK_DELETE_NAME);

    luaL_register(state, "df", no_functions);

    {
        // Assign df a metatable with read-only contents
        lua_newtable(state);

        // Render the type structure
        RenderTypeChildren(state, compound_identity::getTopScope());

        lua_getfield(state, LUA_REGISTRYINDEX, DFHACK_SIZEOF_NAME);
        lua_setfield(state, -2, "sizeof");
        lua_getfield(state, LUA_REGISTRYINDEX, DFHACK_NEW_NAME);
        lua_setfield(state, -2, "new");
        lua_getfield(state, LUA_REGISTRYINDEX, DFHACK_DELETE_NAME);
        lua_setfield(state, -2, "delete");
        lua_getfield(state, LUA_REGISTRYINDEX, DFHACK_DISPLACE_NAME);
        lua_setfield(state, -2, "_displace");
        lua_getfield(state, LUA_REGISTRYINDEX, DFHACK_ASSIGN_NAME);
        lua_setfield(state, -2, "assign");

        lua_pushlightuserdata(state, NULL);
        lua_setfield(state, -2, "NULL");
        lua_pushlightuserdata(state, NULL);
        lua_setglobal(state, "NULL");

        freeze_table(state, true, "df");
        lua_remove(state, -2);
        lua_setmetatable(state, -2);
    }

    lua_pop(state, 1);
}

/**
 * Initialize access to DF objects from the interpreter
 * context, unless it has already been done.
 */
void DFHack::AttachDFGlobals(lua_State *state)
{
    if (luaL_newmetatable(state, DFHACK_TYPETABLE_NAME))
        DoAttach(state);

    lua_pop(state, 1);
}
