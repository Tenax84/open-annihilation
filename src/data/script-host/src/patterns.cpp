// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// string.find, string.match, string.gmatch and string.gsub, following
// lstrlib.c of Lua 5.4.9.
// Copyright (C) 1994-2026 Lua.org, PUC-Rio.
// The licence is the MIT licence; the text is licenses/lua-LICENSE.txt.
// The step count is the part this file adds: each entry of the matcher, each
// character a repetition tries, and each character a balanced pair scans, is
// added to the phase's instruction count. Past the allowance the call raises
// the same budget message as an endless loop.
//
// The functions are C calls that Lua leaves by a long jump. No object with
// a destructor is alive across any of those calls.

#include "internal.hpp"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace oa::data::script_host {
namespace {

constexpr int capture_unfinished = -1;
constexpr int capture_position = -2;
constexpr int max_captures = 32;

/// Most nested entries the matcher may make. One more is "pattern too complex".
constexpr int MAXCCALLS = 200;

constexpr char escape = '%';
constexpr const char* specials = "^$*+?.([%-";

struct MatchState {
    const char* src_init{};
    const char* src_end{};
    const char* p_end{};
    lua_State* L{};
    int matchdepth{};
    unsigned char level{};

    struct {
        const char* init{};
        ptrdiff_t len{};
    } capture[max_captures];
};

struct GMatchState {
    const char* src{};
    const char* p{};
    const char* lastmatch{};
    MatchState ms{};
};

/// Adds one matcher step to the phase count and raises when the allowance is spent.
void charge_step(lua_State* state) {
    charge(state, 1);
}

constexpr const char* gsub_limit_message =
    "string.gsub would make a string of more than 65536 bytes";

/// Refuses a replacement that would pass the string cap, then appends `bytes`.
void add_limited(lua_State* state, luaL_Buffer* buffer, const char* bytes, std::size_t count) {
    reject_long_piece(state, luaL_bufflen(buffer), count, gsub_limit_message);
    luaL_addlstring(buffer, bytes, count);
}

/// Refuses a replacement that would pass the string cap, then appends one byte.
void add_limited_byte(lua_State* state, luaL_Buffer* buffer, char byte) {
    reject_long_piece(state, luaL_bufflen(buffer), 1, gsub_limit_message);
    luaL_addchar(buffer, byte);
}

/// Refuses a replacement that would pass the string cap, then appends the top string.
void add_limited_value(lua_State* state, luaL_Buffer* buffer) {
    std::size_t count = 0;
    lua_tolstring(state, -1, &count);
    reject_long_piece(state, luaL_bufflen(buffer), count, gsub_limit_message);
    luaL_addvalue(buffer);
}

/// Returns one byte as a value the character classes can test.
int byte_of(char text) {
    return static_cast<unsigned char>(text);
}

/// Checks that capture `l` (a character `'1'`..`'9'`) is finished.
int check_capture(MatchState* ms, int l) {
    l -= '1';
    if (l < 0 || l >= ms->level || ms->capture[l].len == capture_unfinished)
        return luaL_error(ms->L, "invalid capture index %%%d", l + 1);
    return l;
}

/// Returns the open capture this `)` closes.
int capture_to_close(MatchState* ms) {
    int level = ms->level;
    for (level--; level >= 0; level--) {
        if (ms->capture[level].len == capture_unfinished)
            return level;
    }
    return luaL_error(ms->L, "invalid pattern capture");
}

/// Returns the address just after the class or character at `p`.
const char* classend(MatchState* ms, const char* p) {
    switch (*p++) {
    case escape: {
        if (p == ms->p_end)
            luaL_error(ms->L, "malformed pattern (ends with '%%')");
        return p + 1;
    }
    case '[': {
        if (*p == '^')
            p++;
        do {
            if (p == ms->p_end)
                luaL_error(ms->L, "malformed pattern (missing ']')");
            if (*p++ == escape && p < ms->p_end)
                p++;
        } while (*p != ']');
        return p + 1;
    }
    default:
        return p;
    }
}

/// Reports whether `c` is in the letter class `cl`, or is the character `cl`.
int match_class(int c, int cl) {
    int res = 0;
    switch (std::tolower(cl)) {
    case 'a':
        res = std::isalpha(c);
        break;
    case 'c':
        res = std::iscntrl(c);
        break;
    case 'd':
        res = std::isdigit(c);
        break;
    case 'g':
        res = std::isgraph(c);
        break;
    case 'l':
        res = std::islower(c);
        break;
    case 'p':
        res = std::ispunct(c);
        break;
    case 's':
        res = std::isspace(c);
        break;
    case 'u':
        res = std::isupper(c);
        break;
    case 'w':
        res = std::isalnum(c);
        break;
    case 'x':
        res = std::isxdigit(c);
        break;
    case 'z':
        res = (c == 0);
        break;
    default:
        return cl == c;
    }
    return std::islower(cl) != 0 ? res : !res;
}

/// Reports whether `c` is in the bracket class from `p` to `ec`.
int matchbracketclass(int c, const char* p, const char* ec) {
    int sig = 1;
    if (*(p + 1) == '^') {
        sig = 0;
        p++;
    }
    while (++p < ec) {
        if (*p == escape) {
            p++;
            if (match_class(c, byte_of(*p)) != 0)
                return sig;
        } else if (*(p + 1) == '-' && (p + 2 < ec)) {
            p += 2;
            if (byte_of(*(p - 2)) <= c && c <= byte_of(*p))
                return sig;
        } else if (byte_of(*p) == c) {
            return sig;
        }
    }
    return !sig;
}

/// Reports whether the character at `s` matches the class from `p` to `ep`.
int singlematch(MatchState* ms, const char* s, const char* p, const char* ep) {
    if (s >= ms->src_end)
        return 0;
    const int c = byte_of(*s);
    switch (*p) {
    case '.':
        return 1;
    case escape:
        return match_class(c, byte_of(*(p + 1)));
    case '[':
        return matchbracketclass(c, p, ep - 1);
    default:
        return byte_of(*p) == c;
    }
}

/// Matches `%bxy` starting at `s`, counting each character it scans.
const char* matchbalance(MatchState* ms, const char* s, const char* p) {
    if (p >= ms->p_end - 1)
        luaL_error(ms->L, "malformed pattern (missing arguments to '%%b')");
    if (*s != *p) {
        charge_step(ms->L);
        return nullptr;
    }
    const int open = *p;
    const int close = *(p + 1);
    int depth = 1;
    charge_step(ms->L);
    while (++s < ms->src_end) {
        charge_step(ms->L);
        if (*s == close) {
            if (--depth == 0)
                return s + 1;
        } else if (*s == open) {
            ++depth;
        }
    }
    return nullptr;
}

const char* match(MatchState* ms, const char* s, const char* p);

/// Takes as many copies as it can, then gives them back one character at a time.
const char* max_expand(MatchState* ms, const char* s, const char* p, const char* ep) {
    ptrdiff_t i = 0;
    for (;;) {
        charge_step(ms->L);
        if (singlematch(ms, s + i, p, ep) == 0)
            break;
        i++;
    }
    while (i >= 0) {
        const char* res = match(ms, s + i, ep + 1);
        if (res != nullptr)
            return res;
        i--;
    }
    return nullptr;
}

/// Takes as few copies as it can, trying one more character after each failure.
const char* min_expand(MatchState* ms, const char* s, const char* p, const char* ep) {
    for (;;) {
        const char* res = match(ms, s, ep + 1);
        if (res != nullptr)
            return res;
        charge_step(ms->L);
        if (singlematch(ms, s, p, ep) != 0)
            s++;
        else
            return nullptr;
    }
}

/// Opens a capture and continues at `p`.
const char* start_capture(MatchState* ms, const char* s, const char* p, int what) {
    const int level = ms->level;
    if (level >= max_captures)
        luaL_error(ms->L, "too many captures");
    ms->capture[level].init = s;
    ms->capture[level].len = what;
    ms->level = static_cast<unsigned char>(level + 1);
    const char* res = match(ms, s, p);
    if (res == nullptr)
        ms->level--;
    return res;
}

/// Closes the innermost open capture and continues at `p`.
const char* end_capture(MatchState* ms, const char* s, const char* p) {
    const int l = capture_to_close(ms);
    ms->capture[l].len = s - ms->capture[l].init;
    const char* res = match(ms, s, p);
    if (res == nullptr)
        ms->capture[l].len = capture_unfinished;
    return res;
}

/// Matches the text of a finished capture again.
const char* match_capture(MatchState* ms, const char* s, int l) {
    l = check_capture(ms, l);
    const auto len = static_cast<size_t>(ms->capture[l].len);
    if (static_cast<size_t>(ms->src_end - s) >= len &&
        std::memcmp(ms->capture[l].init, s, len) == 0)
        return s + len;
    return nullptr;
}

/// Matches `p` at `s`. One step is counted on entry. Depth starts again at `MAXCCALLS`.
const char* match(MatchState* ms, const char* s, const char* p) {
    charge_step(ms->L);
    if (ms->matchdepth-- == 0)
        luaL_error(ms->L, "pattern too complex");
init:
    if (p != ms->p_end) {
        switch (*p) {
        case '(': {
            if (*(p + 1) == ')')
                s = start_capture(ms, s, p + 2, capture_position);
            else
                s = start_capture(ms, s, p + 1, capture_unfinished);
            break;
        }
        case ')': {
            s = end_capture(ms, s, p + 1);
            break;
        }
        case '$': {
            if ((p + 1) != ms->p_end)
                goto dflt;
            s = (s == ms->src_end) ? s : nullptr;
            break;
        }
        case escape: {
            switch (*(p + 1)) {
            case 'b': {
                s = matchbalance(ms, s, p + 2);
                if (s != nullptr) {
                    p += 4;
                    goto init;
                }
                break;
            }
            case 'f': {
                p += 2;
                if (*p != '[')
                    luaL_error(ms->L, "missing '[' after '%%f' in pattern");
                const char* ep = classend(ms, p);
                const char previous = (s == ms->src_init) ? '\0' : *(s - 1);
                if (matchbracketclass(byte_of(previous), p, ep - 1) == 0 &&
                    matchbracketclass(byte_of(*s), p, ep - 1) != 0) {
                    p = ep;
                    goto init;
                }
                s = nullptr;
                break;
            }
            case '0':
            case '1':
            case '2':
            case '3':
            case '4':
            case '5':
            case '6':
            case '7':
            case '8':
            case '9': {
                s = match_capture(ms, s, byte_of(*(p + 1)));
                if (s != nullptr) {
                    p += 2;
                    goto init;
                }
                break;
            }
            default:
                goto dflt;
            }
            break;
        }
        default:
        dflt:
            {
                const char* ep = classend(ms, p);
                if (singlematch(ms, s, p, ep) == 0) {
                    if (*ep == '*' || *ep == '?' || *ep == '-') {
                        p = ep + 1;
                        goto init;
                    }
                    s = nullptr;
                } else {
                    switch (*ep) {
                    case '?': {
                        const char* res = match(ms, s + 1, ep + 1);
                        if (res != nullptr)
                            s = res;
                        else {
                            p = ep + 1;
                            goto init;
                        }
                        break;
                    }
                    case '+':
                        s++;
                        [[fallthrough]];
                    case '*':
                        s = max_expand(ms, s, p, ep);
                        break;
                    case '-':
                        s = min_expand(ms, s, p, ep);
                        break;
                    default:
                        s++;
                        p = ep;
                        goto init;
                    }
                }
                break;
            }
        }
    }
    ms->matchdepth++;
    return s;
}

/// Finds `s2` inside `s1`, or returns null.
///
/// Charges each `memchr` scan and each `memcmp` as it goes. An empty pattern
/// does not scan. `charge` may raise, so nothing here has a destructor.
const char* lmemfind(lua_State* state, const char* s1, size_t l1, const char* s2, size_t l2) {
    if (l2 == 0)
        return s1;
    if (l2 > l1)
        return nullptr;
    l2--;
    l1 = l1 - l2;
    while (l1 > 0) {
        const void* found = std::memchr(s1, byte_of(*s2), l1);
        std::size_t looked = l1;
        if (found != nullptr) {
            const auto* hit = static_cast<const char*>(found);
            looked = static_cast<std::size_t>(hit - s1) + 1;
        }
        charge(state, units_for_bytes(looked));
        if (found == nullptr)
            return nullptr;
        const auto* init = static_cast<const char*>(found);
        init++;
        charge(state, units_for_bytes(l2));
        if (std::memcmp(init, s2 + 1, l2) == 0)
            return init - 1;
        l1 -= static_cast<size_t>(init - s1);
        s1 = init;
    }
    return nullptr;
}

/// Pushes capture `i`, or the whole match when there is no such capture.
ptrdiff_t get_onecapture(MatchState* ms, int i, const char* s, const char* e, const char** cap) {
    if (i >= ms->level) {
        if (i != 0)
            luaL_error(ms->L, "invalid capture index %%%d", i + 1);
        *cap = s;
        return e - s;
    }
    const ptrdiff_t capl = ms->capture[i].len;
    *cap = ms->capture[i].init;
    if (capl == capture_unfinished)
        luaL_error(ms->L, "unfinished capture");
    else if (capl == capture_position)
        lua_pushinteger(ms->L, (ms->capture[i].init - ms->src_init) + 1);
    return capl;
}

/// Pushes capture `i` on the stack.
void push_onecapture(MatchState* ms, int i, const char* s, const char* e) {
    const char* cap = nullptr;
    const ptrdiff_t length = get_onecapture(ms, i, s, e, &cap);
    if (length != capture_position)
        lua_pushlstring(ms->L, cap, static_cast<size_t>(length));
}

/// Pushes every capture and returns how many values that was.
int push_captures(MatchState* ms, const char* s, const char* e) {
    const int nlevels = (ms->level == 0 && s != nullptr) ? 1 : ms->level;
    luaL_checkstack(ms->L, nlevels, "too many captures");
    for (int i = 0; i < nlevels; i++)
        push_onecapture(ms, i, s, e);
    return nlevels;
}

/// Reports whether `p` holds none of the pattern specials.
int nospecials(const char* p, size_t length) {
    size_t upto = 0;
    do {
        if (std::strpbrk(p + upto, specials) != nullptr)
            return 0;
        upto += std::strlen(p + upto) + 1;
    } while (upto <= length);
    return 1;
}

/// Clips a start position into the string, with 1 meaning the first byte.
size_t posrelat(lua_Integer pos, size_t len) {
    if (pos > 0)
        return static_cast<size_t>(pos);
    if (pos == 0)
        return 1;
    if (pos < -static_cast<lua_Integer>(len))
        return 1;
    return len + static_cast<size_t>(pos) + 1;
}

/// Records the subject and the pattern. The depth is set by `reprepstate`.
void prepstate(MatchState* ms, lua_State* L, const char* s, size_t ls, const char* p, size_t lp) {
    ms->L = L;
    ms->src_init = s;
    ms->src_end = s + ls;
    ms->p_end = p + lp;
}

/// Starts one attempt: full depth, no captures.
void reprepstate(MatchState* ms) {
    ms->matchdepth = MAXCCALLS;
    ms->level = 0;
}

/// Implements string.find when `finding` is set, and string.match otherwise.
int str_find_aux(lua_State* L, int finding) {
    size_t ls = 0;
    size_t lp = 0;
    const char* s = luaL_checklstring(L, 1, &ls);
    const char* p = luaL_checklstring(L, 2, &lp);
    const size_t init = posrelat(luaL_optinteger(L, 3, 1), ls) - 1;
    if (init > ls) {
        luaL_pushfail(L);
        return 1;
    }
    if (finding != 0 && (lua_toboolean(L, 4) != 0 || nospecials(p, lp) != 0)) {
        const char* const haystack = s + init;
        const std::size_t haystack_length = ls - init;
        const char* s2 = lmemfind(L, haystack, haystack_length, p, lp);
        if (s2 != nullptr) {
            lua_pushinteger(L, static_cast<lua_Integer>(s2 - s) + 1);
            lua_pushinteger(L, static_cast<lua_Integer>(s2 - s) + static_cast<lua_Integer>(lp));
            return 2;
        }
    } else {
        MatchState ms;
        const char* s1 = s + init;
        const int anchor = (*p == '^') ? 1 : 0;
        if (anchor != 0) {
            p++;
            lp--;
        }
        prepstate(&ms, L, s, ls, p, lp);
        do {
            reprepstate(&ms);
            const char* res = match(&ms, s1, p);
            if (res != nullptr) {
                if (finding != 0) {
                    lua_pushinteger(L, static_cast<lua_Integer>(s1 - s) + 1);
                    lua_pushinteger(L, static_cast<lua_Integer>(res - s));
                    return push_captures(&ms, nullptr, nullptr) + 2;
                }
                return push_captures(&ms, s1, res);
            }
        } while (s1++ < ms.src_end && anchor == 0);
    }
    luaL_pushfail(L);
    return 1;
}

/// Finds the pattern and returns the indexes, plus any captures.
int str_find(lua_State* L) {
    return str_find_aux(L, 1);
}

/// Returns the captures, or the whole match when the pattern has none.
int str_match(lua_State* L) {
    return str_find_aux(L, 0);
}

extern "C" {
/// Continues one gmatch iteration from the userdata upvalue.
static int gmatch_aux(lua_State* L) {
    auto* gm = static_cast<GMatchState*>(lua_touserdata(L, lua_upvalueindex(3)));
    gm->ms.L = L;
    for (const char* src = gm->src; src <= gm->ms.src_end; src++) {
        reprepstate(&gm->ms);
        const char* e = match(&gm->ms, src, gm->p);
        if (e != nullptr && e != gm->lastmatch) {
            gm->src = gm->lastmatch = e;
            return push_captures(&gm->ms, src, e);
        }
    }
    return 0;
}
} // extern "C"

/// Returns an iterator that keeps its place in a userdata value.
int str_gmatch(lua_State* L) {
    size_t ls = 0;
    size_t lp = 0;
    const char* s = luaL_checklstring(L, 1, &ls);
    const char* p = luaL_checklstring(L, 2, &lp);
    size_t init = posrelat(luaL_optinteger(L, 3, 1), ls) - 1;
    lua_settop(L, 2);
    auto* gm = static_cast<GMatchState*>(lua_newuserdatauv(L, sizeof(GMatchState), 0));
    if (init > ls)
        init = ls + 1;
    prepstate(&gm->ms, L, s, ls, p, lp);
    gm->src = s + init;
    gm->p = p;
    gm->lastmatch = nullptr;
    lua_pushcclosure(L, gmatch_aux, 3);
    return 1;
}

/// Copies the replacement string, expanding `%0`, `%1`..`%9` and `%%`.
void add_s(MatchState* ms, luaL_Buffer* b, const char* s, const char* e) {
    size_t l = 0;
    lua_State* L = ms->L;
    const char* news = lua_tolstring(L, 3, &l);
    while (const void* found = std::memchr(news, escape, l)) {
        const auto* cursor = static_cast<const char*>(found);
        add_limited(L, b, news, static_cast<size_t>(cursor - news));
        cursor++;
        if (*cursor == escape) {
            add_limited_byte(L, b, *cursor);
        } else if (*cursor == '0') {
            add_limited(L, b, s, static_cast<size_t>(e - s));
        } else if (std::isdigit(byte_of(*cursor)) != 0) {
            const char* cap = nullptr;
            const ptrdiff_t resl = get_onecapture(ms, *cursor - '1', s, e, &cap);
            if (resl == capture_position)
                add_limited_value(L, b);
            else
                add_limited(L, b, cap, static_cast<size_t>(resl));
        } else {
            luaL_error(L, "invalid use of '%c' in replacement string", escape);
        }
        l -= static_cast<size_t>(cursor + 1 - news);
        news = cursor + 1;
    }
    add_limited(L, b, news, l);
}

/// Appends the replacement for one match. Returns 1 when the text changes.
int add_value(MatchState* ms, luaL_Buffer* b, const char* s, const char* e, int tr) {
    lua_State* L = ms->L;
    switch (tr) {
    case LUA_TFUNCTION: {
        lua_pushvalue(L, 3);
        const int n = push_captures(ms, s, e);
        lua_call(L, n, 1);
        break;
    }
    case LUA_TTABLE: {
        push_onecapture(ms, 0, s, e);
        lua_gettable(L, 3);
        break;
    }
    default: {
        add_s(ms, b, s, e);
        return 1;
    }
    }
    if (lua_toboolean(L, -1) == 0) {
        lua_pop(L, 1);
        add_limited(L, b, s, static_cast<size_t>(e - s));
        return 0;
    }
    if (lua_isstring(L, -1) == 0)
        return luaL_error(L, "invalid replacement value (a %s)", luaL_typename(L, -1));
    add_limited_value(L, b);
    return 1;
}

/// Replaces each match, up to the count given as the fourth argument.
int str_gsub(lua_State* L) {
    size_t srcl = 0;
    size_t lp = 0;
    const char* src = luaL_checklstring(L, 1, &srcl);
    const char* p = luaL_checklstring(L, 2, &lp);
    const char* lastmatch = nullptr;
    const int tr = lua_type(L, 3);
    const lua_Integer max_s = luaL_optinteger(L, 4, static_cast<lua_Integer>(srcl) + 1);
    const int anchor = (*p == '^') ? 1 : 0;
    lua_Integer n = 0;
    int changed = 0;
    MatchState ms;
    luaL_Buffer buffer;
    luaL_argexpected(
        L,
        tr == LUA_TNUMBER || tr == LUA_TSTRING || tr == LUA_TFUNCTION || tr == LUA_TTABLE,
        3,
        "string/function/table"
    );
    luaL_buffinit(L, &buffer);
    if (anchor != 0) {
        p++;
        lp--;
    }
    prepstate(&ms, L, src, srcl, p, lp);
    while (n < max_s) {
        reprepstate(&ms);
        const char* e = match(&ms, src, p);
        if (e != nullptr && e != lastmatch) {
            n++;
            changed = add_value(&ms, &buffer, src, e, tr) | changed;
            src = lastmatch = e;
        } else if (src < ms.src_end) {
            add_limited_byte(L, &buffer, *src++);
        } else {
            break;
        }
        if (anchor != 0)
            break;
    }
    if (changed == 0)
        lua_pushvalue(L, 1);
    else {
        add_limited(L, &buffer, src, static_cast<size_t>(ms.src_end - src));
        luaL_pushresult(&buffer);
    }
    lua_pushinteger(L, n);
    return 2;
}

// Lua calls these through its C function type; they stay file-local.
extern "C" {
static int counted_find(lua_State* state) {
    return str_find(state);
}

static int counted_match(lua_State* state) {
    return str_match(state);
}

static int counted_gmatch(lua_State* state) {
    return str_gmatch(state);
}

static int counted_gsub(lua_State* state) {
    return str_gsub(state);
}
} // extern "C"

} // namespace

void install_patterns(lua_State* state) {
    lua_pushcfunction(state, counted_find);
    lua_setfield(state, -2, "find");
    lua_pushcfunction(state, counted_match);
    lua_setfield(state, -2, "match");
    lua_pushcfunction(state, counted_gmatch);
    lua_setfield(state, -2, "gmatch");
    lua_pushcfunction(state, counted_gsub);
    lua_setfield(state, -2, "gsub");
}

} // namespace oa::data::script_host
