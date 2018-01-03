
#include "../cbits/hexml.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>

/////////////////////////////////////////////////////////////////////
// TYPES

static inline int end(str s) { return s.start + s.length; }

static inline str start_length(int32_t start, int32_t length)
{
    assert(start >= 0 && length >= 0);
    str res;
    res.start = start;
    res.length = length;
    return res;
}

static inline str start_end(int32_t start, int32_t end)
{
    return start_length(start, end - start);
}

typedef struct
{
    int size;
    int used;
    attr* attrs; // dynamically allocated buffer
    attr* alloc; // what to call free on
} attr_buffer;

typedef struct
{
    // have a cursor at the front, which is all the stuff I have written out, final
    // have a cursor at the end, which is stack scoped, so children write out, then I do
    // when you commit, you copy over from end to front

    // nodes
    int size;
    int used_front; // front entries, stored for good
    int used_back; // back entries, stack based, copied into front
    node* nodes; // dynamically allocated buffer
    node* alloc; // what to call free on
} node_buffer;


struct document
{
    const char* body; // pointer to initial argument, not owned by us
    int body_len; // length of the body

    char* error_message;
    node_buffer nodes;
    attr_buffer attrs;
};


/////////////////////////////////////////////////////////////////////
// RENDER CODE

typedef struct
{
    const document* d; // Document, used to get at the body
    char* buffer; // Buffer we are writing too
    int length; // Length of buffer, do not write past the end.
                // No character is reserved for a NULL terminator.
    int cursor; // Cursor of where we are writing now
} render;

static inline void render_char(render* r, char c)
{
    if (r->cursor < r->length)
        r->buffer[r->cursor] = c;
    r->cursor++;
}

static inline void bound(int idx, int mn, int mx)
{
    assert(idx >= mn && idx <= mx);
}

static inline void bound_str(str s, int mn, int mx)
{
    assert(s.length >= 0);
    bound(s.start, mn, mx);
    bound(end(s), mn, mx);
}

static void render_str(render* r, str s)
{
    bound_str(s, 0, r->d->body_len);
    for (int i = 0; i < s.length; i++)
        render_char(r, r->d->body[s.start + i]);
}

static void render_tag(render* r, const node* n);

static void render_content(render* r, const node* n)
{
    bound_str(n->inner, 0, r->d->body_len);
    bound_str(n->nodes, 0, r->d->nodes.used_front);
    bound_str(n->attrs, 0, r->d->attrs.used);

    int done = n->inner.start;
    for (int i = 0; i < n->nodes.length; i++)
    {
        node* x = &r->d->nodes.nodes[n->nodes.start + i];
        render_str(r, start_end(done, x->outer.start));
        done = end(x->outer);
        render_tag(r, x);
    }
    render_str(r, start_end(done, end(n->inner)));
}

static void render_tag(render* r, const node* n)
{
    render_char(r, '<');
    render_str(r, n->name);
    for (int i = 0; i < n->attrs.length; i++)
    {
        attr* x = &r->d->attrs.attrs[n->attrs.start + i];
        render_char(r, ' ');
        render_str(r, x->name);
        render_char(r, '=');
        render_char(r, '\"');
        render_str(r, x->value);
        render_char(r, '\"');
    }
    render_char(r, '>');
    render_content(r, n);
    render_char(r, '<');
    render_char(r, '/');
    render_str(r, n->name);
    render_char(r, '>');
}

int hexml_node_render(const document* d, const node* n, char* buffer, int length)
{
    render r;
    r.d = d;
    r.buffer = buffer;
    r.length = length;
    r.cursor = 0;
    // The root node (and only the root node) has an empty length, so just render its innards
    if (n->name.length == 0)
        render_content(&r, n);
    else
        render_tag(&r, n);
    return r.cursor;
}


/////////////////////////////////////////////////////////////////////
// NOT THE PARSER

char* hexml_document_error(const document* d){return d->error_message;}
node* hexml_document_node(const document* d){return &d->nodes.nodes[0];}

void hexml_document_free(document* d)
{
    free(d->error_message);
    free(d->nodes.alloc);
    free(d->attrs.alloc);
    free(d);
}

node* hexml_node_children(const document* d, const node* n, int* res)
{
    *res = n->nodes.length;
    return &d->nodes.nodes[n->nodes.start];
}

attr* hexml_node_attributes(const document* d, const node* n, int* res)
{
    *res = n->attrs.length;
    return &d->attrs.attrs[n->attrs.start];
}

attr* hexml_node_attribute(const document* d, const node* n, const char* s, int slen)
{
    if (slen == -1) slen = (int) strlen(s);
    const int limit = end(n->attrs);
    for (int i = n->attrs.start; i < limit; i++)
    {
        attr* r = &d->attrs.attrs[i];
        if (r->name.length == slen && memcmp(s, &d->body[r->name.start], slen) == 0)
            return r;
    }
    return NULL;
}

// Search for given strings within a node
node* hexml_node_child(const document* d, const node* parent, const node* prev, const char* s, int slen)
{
    if (slen == -1) slen = (int) strlen(s);
    int i = prev == NULL ? parent->nodes.start : (int) (prev + 1 - d->nodes.nodes);
    const int limit = end(parent->nodes);
    for (; i < limit; i++)
    {
        node* r = &d->nodes.nodes[i];
        if (r->name.length == slen && memcmp(s, &d->body[r->name.start], slen) == 0)
            return r;
    }
    return NULL;
}

/////////////////////////////////////////////////////////////////////
// PARSING CODE

static inline void node_alloc(node_buffer* b, int ask)
{
    int space = b->size - b->used_back - b->used_front;
    if (space >= ask) return;
    int size2 = (b->size + 1000 + ask) * 2;
    node* buf2 = malloc(size2 * sizeof(node));
    assert(buf2);
    memcpy(buf2, b->nodes, b->used_front * sizeof(node));
    memcpy(&buf2[size2 - b->used_back], &b->nodes[b->size - b->used_back], b->used_back * sizeof(node));
    free(b->alloc);
    b->size = size2;
    b->nodes = buf2;
    b->alloc = buf2;
}

static inline void attr_alloc(attr_buffer* b, int ask)
{
    int space = b->size - b->used;
    if (space >= ask) return;
    int size2 = (b->size + 1000 + ask) * 2;
    attr* buf2 = malloc(size2 * sizeof(attr));
    assert(buf2);
    memcpy(buf2, b->attrs, b->used * sizeof(attr));
    free(b->alloc);
    b->size = size2;
    b->attrs = buf2;
    b->alloc = buf2;
}

static void set_error(document* d, const char* msg)
{
    if (d->error_message != NULL) return; // keep the first error message
    d->error_message = malloc(strlen(msg)+1);
    assert(d->error_message);
    strcpy(d->error_message, msg);
}

static inline str gap(const char* ref, const char* start, const char* end)
{
    return start_end(start - ref, end - ref);
}

#define P_Abort(x) return x
#define P_NameStart name_start = p
#define P_NameEnd name_end = p
#define P_QuoteStart quote_start = p
#define P_QuoteEnd \
    quote_end = p; \
    attr_alloc(&d->attrs, 1); \
    d->attrs.attrs[d->attrs.used].name = gap(d->body, name_start, name_end); \
    d->attrs.attrs[d->attrs.used].value = gap(d->body, quote_start, quote_end); \
    d->attrs.used++;
#define P_AttribsStart printf("AttribsStart %s\n", p)
#define P_AttribsEnd printf("AttribsEnd %s\n", p)
#define P_Tag tag_start = p
#define P_TagComment printf("TagComment %s\n", p)
#define P_TagOpen printf("TagOpen %s\n", p)
#define P_TagClose printf("TagClose %s\n", p)
#define P_TagOpenClose \
    d->nodes.nodes[d->nodes.used_front].outer = gap(d->body, tag_start, p); \
    printf("TagOpenClose %s\n", p)

static const char* parser(const char* p, document* d)
{
    const char *tag_start, *name_start, *name_end, *quote_start, *quote_end;
#   include "test.h"
    return NULL;
}

// Based on looking at ~50Kb XML documents, they seem to have ~700 attributes
// and ~300 nodes, so size appropriately to cope with that.
typedef struct
{
    document document;
    attr attrs[1000];
    node nodes[500];
} buffer;

document* hexml_document_parse(const char* s, int slen)
{
    if (slen == -1) slen = (int) strlen(s);
    assert(s[slen] == 0);
    // init_parse_table();

    buffer* buf = malloc(sizeof(buffer));
    assert(buf);
    document* d = &buf->document;
    d->body = s;
    d->body_len = slen;
    d->error_message = NULL;
    d->attrs.size = 1000;
    d->attrs.used = 0;
    d->attrs.attrs = buf->attrs;
    d->attrs.alloc = NULL;
    d->nodes.size = 500;
    d->nodes.used_back = 0;
    d->nodes.used_front = 1;
    d->nodes.nodes = buf->nodes;
    d->nodes.alloc = NULL;

    d->nodes.nodes[0].name = start_length(0, 0);
    d->nodes.nodes[0].outer = start_length(0, slen);
    d->nodes.nodes[0].inner = start_length(0, slen);
    d->nodes.nodes[0].attrs = start_length(0, 0);

    // Introduce an intermediate result, otherwise behaviour is undefined
    // because there is no guaranteed ordering between LHS and RHS evaluation
    const char* res = parser(s, d);
    if (res != NULL)
        d->error_message = strdup(res);
    // d->nodes.nodes[0].nodes = content;
    return d;
}

/////////////////////////////////////////////////////////////////////
// TEST HARNESS (temporary)

int main()
{
    const document* d = hexml_document_parse("<test foo='123 xyz'><inner /> value</test>", -1);
    char buf[1000];
    int len = hexml_node_render(d, hexml_document_node(d), buf, sizeof(buf));
    buf[len] = 0;
    printf("Result = %s\n", buf);
    printf("Used nodes = %i, attribtes = %i\n", d->nodes.used_front, d->attrs.used);
    return 0;
}

/*
void main()
{
    setbuf(stdout, NULL);
    char* body = "<?xml version=\"1.0\" lang=\"ja\"?><foo><bar baz=\"qux\">quux</bar><bar baz=\"bar\">hoge</bar><piyo>&gt;piyopiyo&lt;</piyo></foo><foo2 />";
    body = "<test>X</test> ";
    document* doc = document_parse(body, -1);
    char* err = document_error(doc);
    if (err == NULL)
    {
        int len = document_render(doc, NULL, 0);
        char* s = malloc(len + 1);
        document_render(doc, s, len);
        s[len] = 0;
        printf("Parse successful, %s\n", s);
    }
    else
        printf("Parse failed, %s\n", err);
}
*/
