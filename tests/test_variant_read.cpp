// test_variant_read.cpp - the read-only views over BMW's Variant / Map / BasicString layouts (bmw_rhmi.hpp) against
// hand-built objects with the same layouts: narrow and wide strings, unknown type codes, integers that must not be
// dereferenced as pointers, and a bucket walk over an MFC-shaped Map.
#include "../src/bmw_rhmi.hpp"
#include "test_util.hpp"
#include <stdint.h>
#include <vector>

namespace {

// A CString-shaped buffer: {refs, length, alloc} then the characters. data() is what BasicString::m_data holds.
struct FakeNarrow {
    int hdr[3];
    char chars[64];
    explicit FakeNarrow(const char* s, int refs = 1) {
        hdr[0] = refs; hdr[1] = (int)strlen(s); hdr[2] = 63;
        memset(chars, 0, sizeof(chars));
        strcpy(chars, s);
    }
    const void* data() const { return chars; }
};
struct FakeWide {
    int hdr[3];
    unsigned short chars[64];
    explicit FakeWide(const char* s) {
        hdr[0] = 1; hdr[1] = (int)strlen(s); hdr[2] = 63;
        memset(chars, 0, sizeof(chars));
        for (int i = 0; s[i]; ++i) chars[i] = (unsigned short)(unsigned char)s[i];
    }
    const void* data() const { return chars; }
};

// A container with a chosen type code whose payload is one pointer-sized word (a BasicString's m_data, or an int).
// The payload has to be the first member after the vptr, as in the framework's ObjectContainer<T>.
class FakeContainer : public bmw::Variant::IObjectContainer {
public:
    FakeContainer(int type, uintptr_t word) : m_word(word), m_type(type) {}
    virtual int type() const { return m_type; }
    virtual IObjectContainer* clone() const { return new FakeContainer(m_type, m_word); }
private:
    uintptr_t m_word;
    int m_type;
};

// bmw::Variant has an undefined (framework) destructor: build it in raw storage and never destroy it.
struct VariantBox {
    unsigned char storage[sizeof(bmw::Variant)];
    bmw::Variant& v() { return *reinterpret_cast<bmw::Variant*>(storage); }
    explicit VariantBox(bmw::Variant::IObjectContainer* c) { memset(storage, 0, sizeof(storage)); v().m_obj = c; }
};

typedef bmw::Map<unsigned, bmw::Variant> ArgMap;

// An MFC-CMap-shaped map built by hand: table of buckets, assoc {next, hash, key, value}. MapShape mirrors
// bmw::Map's members in declaration order, so the compiler gives it the same layout on any ABI.
struct MapShape {
    ArgMap::Assoc** table;
    unsigned hashSize, count;
    void* freeList;
    void* blocks;
    unsigned blockSize;
};
struct FakeMap {
    ArgMap::Assoc* table[17];
    std::vector<ArgMap::Assoc*> owned;
    MapShape shape;
    FakeMap() {
        memset(table, 0, sizeof(table));
        memset(&shape, 0, sizeof(shape));
        shape.table = table;
        shape.hashSize = 17;
        shape.blockSize = 10;
    }
    void put(unsigned key, bmw::Variant::IObjectContainer* c) {
        ArgMap::Assoc* a = new ArgMap::Assoc;
        memset(static_cast<void*>(a), 0, sizeof(*a));
        unsigned bucket = ((key >> 4) | (key << 28)) % 17;    // ror(key, 4) % hashSize, as the framework hashes
        a->next = table[bucket];
        a->hash = bucket;
        a->key = key;
        a->value.m_obj = c;
        table[bucket] = a;
        owned.push_back(a);
        shape.count++;
    }
    const ArgMap& map() const { return *reinterpret_cast<const ArgMap*>(&shape); }
};

} // namespace

int main() {
    char out[64];
    {   // narrow string, known type 0x22
        FakeNarrow s("ls -la");
        FakeContainer c(bmw::Variant::kTypeString, (uintptr_t)s.data());
        VariantBox v(&c);
        bmw::VariantView vv = bmw::viewVariant(v.v());
        CHECK_EQ(vv.type, 0x22);
        CHECK(bmw::variantToString(v.v(), out, sizeof(out)));
        CHECK_EQ_STR(out, "ls -la");
        int i = 0;
        CHECK(!bmw::variantToInt(v.v(), &i));                  // a string is not an index
    }
    {   // narrow string behind an unknown type code: still read, through the CString header
        FakeNarrow s("del");
        FakeContainer c(0x99, (uintptr_t)s.data());
        VariantBox v(&c);
        CHECK(bmw::variantToString(v.v(), out, sizeof(out)));
        CHECK_EQ_STR(out, "del");
    }
    {   // wide (UTF-16) string behind an unknown type code: detected from the character layout, 1 char and more
        FakeWide w1("a");
        FakeContainer c1(0x23, (uintptr_t)w1.data());
        VariantBox v1(&c1);
        CHECK(bmw::variantToString(v1.v(), out, sizeof(out)));
        CHECK_EQ_STR(out, "a");
        FakeWide w2("delall");
        FakeContainer c2(0x23, (uintptr_t)w2.data());
        VariantBox v2(&c2);
        CHECK(bmw::variantToString(v2.v(), out, sizeof(out)));
        CHECK_EQ_STR(out, "delall");
        FakeWide w3("");
        FakeContainer c3(0x23, (uintptr_t)w3.data());
        VariantBox v3(&c3);
        CHECK(bmw::variantToString(v3.v(), out, sizeof(out)));
        CHECK_EQ_STR(out, "");
    }
    {   // non-ASCII in a wide string comes out as UTF-8
        FakeWide w("x");
        w.chars[0] = 0x00e9;                                   // e-acute
        FakeContainer c(0x23, (uintptr_t)w.data());
        VariantBox v(&c);
        CHECK(bmw::variantToString(v.v(), out, sizeof(out)));
        CHECK_EQ_STR(out, "\xc3\xa9");
    }
    {   // small integers (a list index) must not be dereferenced as pointers, and read back as ints
        FakeContainer c(0x05, 3);
        VariantBox v(&c);
        CHECK(!bmw::variantToString(v.v(), out, sizeof(out)));
        int i = -1;
        CHECK(bmw::variantToInt(v.v(), &i));
        CHECK_EQ(i, 3);
    }
    {   // a word that looks like an address but is unmapped: the probe says no, no crash
        FakeContainer c(0x23, (uintptr_t)0x7fff0000u);
        VariantBox v(&c);
        CHECK(!bmw::variantToString(v.v(), out, sizeof(out)));
        FakeContainer c2(0x23, (uintptr_t)4096);
        VariantBox v2(&c2);
        CHECK(!bmw::variantToString(v2.v(), out, sizeof(out)));
    }
    {   // a mapped pointer to something that is not a CString (header fails the plausibility checks)
        int junk[8] = { 0x7fffffff, -5, 0, 0, 0, 0, 0, 0 };
        FakeContainer c(0x23, (uintptr_t)&junk[3]);
        VariantBox v(&c);
        CHECK(!bmw::variantToString(v.v(), out, sizeof(out)));
    }
    {   // an empty Variant
        VariantBox v(0);
        CHECK(!bmw::variantToString(v.v(), out, sizeof(out)));
        int i;
        CHECK(!bmw::variantToInt(v.v(), &i));
        CHECK_EQ(bmw::viewVariant(v.v()).type, -1);
    }
    {   // output truncation
        FakeNarrow s("0123456789");
        FakeContainer c(bmw::Variant::kTypeString, (uintptr_t)s.data());
        VariantBox v(&c);
        char small[5];
        CHECK(bmw::variantToString(v.v(), small, sizeof(small)));
        CHECK_EQ_STR(small, "0123");
    }
    {   // Map::find / forEach over a hand-built MFC-shaped map: keys 8 (letter) and 1 (index), plus a miss
        FakeNarrow s("k");
        FakeContainer letter(bmw::Variant::kTypeString, (uintptr_t)s.data());
        FakeContainer index(0x05, 2);
        FakeMap m;
        m.put(8, &letter);
        m.put(1, &index);
        m.put(25, &index);                                      // 25 = ror(1,4)? no: just a second entry in a bucket chain
        CHECK_EQ(m.map().count(), 3);
        const bmw::Variant* v8 = m.map().find(8);
        CHECK(v8 != 0);
        CHECK(v8 && bmw::variantToString(*v8, out, sizeof(out)));
        CHECK_EQ_STR(out, "k");
        const bmw::Variant* v1 = m.map().find(1);
        int i = 0;
        CHECK(v1 && bmw::variantToInt(*v1, &i));
        CHECK_EQ(i, 2);
        CHECK(m.map().find(9) == 0);
        struct Count { unsigned n; unsigned sum; Count() : n(0), sum(0) {} void operator()(const unsigned& k, const bmw::Variant&) { ++n; sum += k; } } cnt;
        m.map().forEach(cnt);
        CHECK_EQ(cnt.n, 3);
        CHECK_EQ(cnt.sum, 8 + 1 + 25);
        unsigned char emptyStorage[sizeof(ArgMap)];             // a zeroed Map (no table yet) finds nothing
        memset(emptyStorage, 0, sizeof(emptyStorage));
        CHECK(reinterpret_cast<const ArgMap*>(emptyStorage)->find(1) == 0);
    }
    return testSummary("test_variant_read");
}
