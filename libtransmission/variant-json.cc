// This file Copyright © 2008-2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <array>
#include <cctype>
#include <cerrno> /* EILSEQ, EINVAL */
#include <cmath> /* fabs() */
#include <cstring>
#include <deque>
#include <string_view>

#define UTF_CPP_CPLUSPLUS 201703L
#include <utf8.h>

#include <event2/buffer.h>

#define LIBTRANSMISSION_VARIANT_MODULE

#include "transmission.h"

#include "jsonsl.h"
#include "log.h"
#include "quark.h"
#include "tr-assert.h"
#include "utils.h"
#include "variant-common.h"
#include "variant.h"

using namespace std::literals;

/* arbitrary value... this is much deeper than our code goes */
static auto constexpr MaxDepth = int{ 64 };

struct json_wrapper_data
{
    bool has_content;
    std::string_view key;
    evbuffer* keybuf;
    evbuffer* strbuf;
    int error;
    std::deque<tr_variant*> stack;
    tr_variant* top;
    int parse_opts;

    /* A very common pattern is for a container's children to be similar,
     * e.g. they may all be objects with the same set of keys. So when
     * a container is popped off the stack, remember its size to use as
     * a preallocation heuristic for the next container at that depth. */
    std::array<size_t, MaxDepth> preallocGuess;
};

static tr_variant* get_node(struct jsonsl_st* jsn)
{
    auto* data = static_cast<struct json_wrapper_data*>(jsn->data);

    auto* parent = std::empty(data->stack) ? nullptr : data->stack.back();

    tr_variant* node = nullptr;
    if (parent == nullptr)
    {
        node = data->top;
    }
    else if (tr_variantIsList(parent))
    {
        node = tr_variantListAdd(parent);
    }
    else if (tr_variantIsDict(parent) && !std::empty(data->key))
    {
        node = tr_variantDictAdd(parent, tr_quark_new(data->key));
        data->key = ""sv;
    }

    return node;
}

static void error_handler(jsonsl_t jsn, jsonsl_error_t error, jsonsl_state_st* /*state*/, jsonsl_char_t const* buf)
{
    auto* data = static_cast<struct json_wrapper_data*>(jsn->data);

    tr_logAddError("JSON parse failed at pos %zu: %s -- remaining text \"%.16s\"", jsn->pos, jsonsl_strerror(error), buf);

    data->error = EILSEQ;
}

static int error_callback(jsonsl_t jsn, jsonsl_error_t error, struct jsonsl_state_st* state, jsonsl_char_t* at)
{
    error_handler(jsn, error, state, at);
    return 0; /* bail */
}

static void action_callback_PUSH(
    jsonsl_t jsn,
    jsonsl_action_t /*action*/,
    struct jsonsl_state_st* state,
    jsonsl_char_t const* /*buf*/)
{
    auto* data = static_cast<struct json_wrapper_data*>(jsn->data);

    if ((state->type == JSONSL_T_LIST) || (state->type == JSONSL_T_OBJECT))
    {
        data->has_content = true;
        tr_variant* node = get_node(jsn);
        data->stack.push_back(node);

        int const depth = std::size(data->stack);
        size_t const n = depth < MaxDepth ? data->preallocGuess[depth] : 0;
        if (state->type == JSONSL_T_LIST)
        {
            tr_variantInitList(node, n);
        }
        else
        {
            tr_variantInitDict(node, n);
        }
    }
}

/* like sscanf(in+2, "%4x", &val) but less slow */
static bool decode_hex_string(char const* in, unsigned int* setme)
{
    TR_ASSERT(in != nullptr);

    unsigned int val = 0;
    char const* const end = in + 6;

    TR_ASSERT(in[0] == '\\');
    TR_ASSERT(in[1] == 'u');
    in += 2;

    do
    {
        val <<= 4;

        if ('0' <= *in && *in <= '9')
        {
            val += *in - '0';
        }
        else if ('a' <= *in && *in <= 'f')
        {
            val += *in - 'a' + 10U;
        }
        else if ('A' <= *in && *in <= 'F')
        {
            val += *in - 'A' + 10U;
        }
        else
        {
            return false;
        }
    } while (++in != end);

    *setme = val;
    return true;
}

static std::string_view extract_escaped_string(char const* in, size_t in_len, struct evbuffer* buf)
{
    char const* const in_end = in + in_len;

    evbuffer_drain(buf, evbuffer_get_length(buf));

    while (in < in_end)
    {
        bool unescaped = false;

        if (*in == '\\' && in_end - in >= 2)
        {
            switch (in[1])
            {
            case 'b':
                evbuffer_add(buf, "\b", 1);
                in += 2;
                unescaped = true;
                break;

            case 'f':
                evbuffer_add(buf, "\f", 1);
                in += 2;
                unescaped = true;
                break;

            case 'n':
                evbuffer_add(buf, "\n", 1);
                in += 2;
                unescaped = true;
                break;

            case 'r':
                evbuffer_add(buf, "\r", 1);
                in += 2;
                unescaped = true;
                break;

            case 't':
                evbuffer_add(buf, "\t", 1);
                in += 2;
                unescaped = true;
                break;

            case '/':
                evbuffer_add(buf, "/", 1);
                in += 2;
                unescaped = true;
                break;

            case '"':
                evbuffer_add(buf, "\"", 1);
                in += 2;
                unescaped = true;
                break;

            case '\\':
                evbuffer_add(buf, "\\", 1);
                in += 2;
                unescaped = true;
                break;

            case 'u':
                {
                    if (in_end - in >= 6)
                    {
                        unsigned int val = 0;

                        if (decode_hex_string(in, &val))
                        {
                            try
                            {
                                auto buf8 = std::array<char, 8>{};
                                auto const it = utf8::append(val, std::data(buf8));
                                evbuffer_add(buf, std::data(buf8), it - std::data(buf8));
                            }
                            catch (utf8::exception const&)
                            { // invalid codepoint
                                evbuffer_add(buf, "?", 1);
                            }
                            unescaped = true;
                            in += 6;
                            break;
                        }
                    }
                }
            }
        }

        if (!unescaped)
        {
            evbuffer_add(buf, in, 1);
            ++in;
        }
    }

    return { (char const*)evbuffer_pullup(buf, -1), evbuffer_get_length(buf) };
}

static std::pair<std::string_view, bool> extract_string(jsonsl_t jsn, struct jsonsl_state_st* state, struct evbuffer* buf)
{
    // figure out where the string is
    char const* in_begin = jsn->base + state->pos_begin;
    if (*in_begin == '"')
    {
        in_begin++;
    }

    char const* const in_end = jsn->base + state->pos_cur;
    size_t const in_len = in_end - in_begin;
    if (memchr(in_begin, '\\', in_len) == nullptr)
    {
        /* it's not escaped */
        return std::make_pair(std::string_view{ in_begin, in_len }, true);
    }

    return std::make_pair(extract_escaped_string(in_begin, in_len, buf), false);
}

static void action_callback_POP(
    jsonsl_t jsn,
    jsonsl_action_t /*action*/,
    struct jsonsl_state_st* state,
    jsonsl_char_t const* /*buf*/)
{
    auto* data = static_cast<struct json_wrapper_data*>(jsn->data);

    if (state->type == JSONSL_T_STRING)
    {
        auto const [str, inplace] = extract_string(jsn, state, data->strbuf);
        if (inplace && ((data->parse_opts & TR_VARIANT_PARSE_INPLACE) != 0))
        {
            tr_variantInitStrView(get_node(jsn), str);
        }
        else
        {
            tr_variantInitStr(get_node(jsn), str);
        }
        data->has_content = true;
    }
    else if (state->type == JSONSL_T_HKEY)
    {
        data->has_content = true;
        auto const [key, inplace] = extract_string(jsn, state, data->keybuf);
        data->key = key;
    }
    else if (state->type == JSONSL_T_LIST || state->type == JSONSL_T_OBJECT)
    {
        int const depth = std::size(data->stack);
        auto* v = data->stack.back();
        data->stack.pop_back();
        if (depth < MaxDepth)
        {
            data->preallocGuess[depth] = v->val.l.count;
        }
    }
    else if (state->type == JSONSL_T_SPECIAL)
    {
        if ((state->special_flags & JSONSL_SPECIALf_NUMNOINT) != 0)
        {
            char const* begin = jsn->base + state->pos_begin;
            data->has_content = true;
            tr_variantInitReal(get_node(jsn), strtod(begin, nullptr));
        }
        else if ((state->special_flags & JSONSL_SPECIALf_NUMERIC) != 0)
        {
            char const* begin = jsn->base + state->pos_begin;
            data->has_content = true;
            tr_variantInitInt(get_node(jsn), std::strtoll(begin, nullptr, 10));
        }
        else if ((state->special_flags & JSONSL_SPECIALf_BOOLEAN) != 0)
        {
            bool const b = (state->special_flags & JSONSL_SPECIALf_TRUE) != 0;
            data->has_content = true;
            tr_variantInitBool(get_node(jsn), b);
        }
        else if ((state->special_flags & JSONSL_SPECIALf_NULL) != 0)
        {
            data->has_content = true;
            tr_variantInitQuark(get_node(jsn), TR_KEY_NONE);
        }
    }
}

int tr_variantParseJson(tr_variant& setme, int parse_opts, std::string_view benc, char const** setme_end)
{
    TR_ASSERT((parse_opts & TR_VARIANT_PARSE_JSON) != 0);

    auto data = json_wrapper_data{};

    jsonsl_t jsn = jsonsl_new(MaxDepth);
    jsn->action_callback_PUSH = action_callback_PUSH;
    jsn->action_callback_POP = action_callback_POP;
    jsn->error_callback = error_callback;
    jsn->data = &data;
    jsonsl_enable_all_callbacks(jsn);

    data.error = 0;
    data.has_content = false;
    data.key = ""sv;
    data.keybuf = evbuffer_new();
    data.parse_opts = parse_opts;
    data.preallocGuess = {};
    data.stack = {};
    data.strbuf = evbuffer_new();
    data.top = &setme;

    /* parse it */
    jsonsl_feed(jsn, static_cast<jsonsl_char_t const*>(std::data(benc)), std::size(benc));

    /* EINVAL if there was no content */
    if (data.error == 0 && !data.has_content)
    {
        data.error = EINVAL;
    }

    /* maybe set the end ptr */
    if (setme_end != nullptr)
    {
        *setme_end = std::data(benc) + jsn->pos;
    }

    /* cleanup */
    int const error = data.error;
    evbuffer_free(data.keybuf);
    evbuffer_free(data.strbuf);
    jsonsl_destroy(jsn);
    return error;
}

/****
*****
****/

struct ParentState
{
    int variantType;
    int childIndex;
    int childCount;
};

struct jsonWalk
{
    bool doIndent;
    std::deque<ParentState> parents;
    struct evbuffer* out;
};

static void jsonIndent(struct jsonWalk* data)
{
    static char buf[1024] = { '\0' };

    if (*buf == '\0')
    {
        memset(buf, ' ', sizeof(buf));
        buf[0] = '\n';
    }

    if (data->doIndent)
    {
        evbuffer_add(data->out, buf, std::size(data->parents) * 4 + 1);
    }
}

static void jsonChildFunc(struct jsonWalk* data)
{
    if (!std::empty(data->parents))
    {
        auto& pstate = data->parents.back();

        switch (pstate.variantType)
        {
        case TR_VARIANT_TYPE_DICT:
            {
                int const i = pstate.childIndex;
                ++pstate.childIndex;

                if (i % 2 == 0)
                {
                    evbuffer_add(data->out, ": ", data->doIndent ? 2 : 1);
                }
                else
                {
                    bool const is_last = pstate.childIndex == pstate.childCount;
                    if (!is_last)
                    {
                        evbuffer_add(data->out, ",", 1);
                        jsonIndent(data);
                    }
                }

                break;
            }

        case TR_VARIANT_TYPE_LIST:
            {
                ++pstate.childIndex;
                if (bool const is_last = pstate.childIndex == pstate.childCount; !is_last)
                {
                    evbuffer_add(data->out, ",", 1);
                    jsonIndent(data);
                }

                break;
            }

        default:
            break;
        }
    }
}

static void jsonPushParent(struct jsonWalk* data, tr_variant const* v)
{
    int const n_children = tr_variantIsDict(v) ? v->val.l.count * 2 : v->val.l.count;
    data->parents.push_back({ v->type, 0, n_children });
}

static void jsonPopParent(struct jsonWalk* data)
{
    data->parents.pop_back();
}

static void jsonIntFunc(tr_variant const* val, void* vdata)
{
    auto* data = static_cast<struct jsonWalk*>(vdata);
    evbuffer_add_printf(data->out, "%" PRId64, val->val.i);
    jsonChildFunc(data);
}

static void jsonBoolFunc(tr_variant const* val, void* vdata)
{
    auto* data = static_cast<struct jsonWalk*>(vdata);

    if (val->val.b)
    {
        evbuffer_add(data->out, "true", 4);
    }
    else
    {
        evbuffer_add(data->out, "false", 5);
    }

    jsonChildFunc(data);
}

static void jsonRealFunc(tr_variant const* val, void* vdata)
{
    auto* data = static_cast<struct jsonWalk*>(vdata);

    if (fabs(val->val.d - (int)val->val.d) < 0.00001)
    {
        evbuffer_add_printf(data->out, "%d", (int)val->val.d);
    }
    else
    {
        evbuffer_add_printf(data->out, "%.4f", tr_truncd(val->val.d, 4));
    }

    jsonChildFunc(data);
}

static void jsonStringFunc(tr_variant const* val, void* vdata)
{
    struct evbuffer_iovec vec[1];
    auto* data = static_cast<struct jsonWalk*>(vdata);

    auto sv = std::string_view{};
    (void)!tr_variantGetStrView(val, &sv);

    evbuffer_reserve_space(data->out, std::size(sv) * 4, vec, 1);
    auto* out = static_cast<char*>(vec[0].iov_base);
    char const* const outend = out + vec[0].iov_len;

    char* outwalk = out;
    *outwalk++ = '"';

    for (; !std::empty(sv); sv.remove_prefix(1))
    {
        switch (sv.front())
        {
        case '\b':
            *outwalk++ = '\\';
            *outwalk++ = 'b';
            break;

        case '\f':
            *outwalk++ = '\\';
            *outwalk++ = 'f';
            break;

        case '\n':
            *outwalk++ = '\\';
            *outwalk++ = 'n';
            break;

        case '\r':
            *outwalk++ = '\\';
            *outwalk++ = 'r';
            break;

        case '\t':
            *outwalk++ = '\\';
            *outwalk++ = 't';
            break;

        case '"':
            *outwalk++ = '\\';
            *outwalk++ = '"';
            break;

        case '\\':
            *outwalk++ = '\\';
            *outwalk++ = '\\';
            break;

        default:
            if (isprint((unsigned char)sv.front()) != 0)
            {
                *outwalk++ = sv.front();
            }
            else
            {
                try
                {
                    auto const* const begin8 = std::data(sv);
                    auto const* const end8 = begin8 + std::size(sv);
                    auto const* walk8 = begin8;
                    auto const uch32 = utf8::next(walk8, end8);
                    outwalk += tr_snprintf(outwalk, outend - outwalk, "\\u%04x", uch32);
                    sv.remove_prefix(walk8 - begin8 - 1);
                }
                catch (utf8::exception const&)
                {
                    *outwalk++ = '?';
                }
            }
            break;
        }
    }

    *outwalk++ = '"';
    vec[0].iov_len = outwalk - out;
    evbuffer_commit_space(data->out, vec, 1);

    jsonChildFunc(data);
}

static void jsonDictBeginFunc(tr_variant const* val, void* vdata)
{
    auto* data = static_cast<struct jsonWalk*>(vdata);

    jsonPushParent(data, val);
    evbuffer_add(data->out, "{", 1);

    if (val->val.l.count != 0)
    {
        jsonIndent(data);
    }
}

static void jsonListBeginFunc(tr_variant const* val, void* vdata)
{
    size_t const nChildren = tr_variantListSize(val);
    auto* data = static_cast<struct jsonWalk*>(vdata);

    jsonPushParent(data, val);
    evbuffer_add(data->out, "[", 1);

    if (nChildren != 0)
    {
        jsonIndent(data);
    }
}

static void jsonContainerEndFunc(tr_variant const* val, void* vdata)
{
    auto* data = static_cast<struct jsonWalk*>(vdata);

    jsonPopParent(data);

    jsonIndent(data);

    if (tr_variantIsDict(val))
    {
        evbuffer_add(data->out, "}", 1);
    }
    else /* list */
    {
        evbuffer_add(data->out, "]", 1);
    }

    jsonChildFunc(data);
}

static struct VariantWalkFuncs const walk_funcs = {
    jsonIntFunc, //
    jsonBoolFunc, //
    jsonRealFunc, //
    jsonStringFunc, //
    jsonDictBeginFunc, //
    jsonListBeginFunc, //
    jsonContainerEndFunc, //
};

void tr_variantToBufJson(tr_variant const* top, struct evbuffer* buf, bool lean)
{
    struct jsonWalk data;

    data.doIndent = !lean;
    data.out = buf;

    tr_variantWalk(top, &walk_funcs, &data, true);

    if (evbuffer_get_length(buf) != 0)
    {
        evbuffer_add_printf(buf, "\n");
    }
}
