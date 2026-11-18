// bmw_rhmi.hpp - minimal declarations of BMW's RHMI app framework (libAppFramework.A.so / libCoreFramework.A.so).
//
// Only names and parameter types matter: they produce the exported, mangled symbols the car's libraries define.
// Nothing declared here is defined by this project (the template members are weak instantiations exported by the
// BMW libraries, so they must NOT be defined here either, or the linker would pick ours).
//
// Everything below was read out of the two libraries with ntoarmv7-nm / objdump (see README "How the ABI was
// derived") and from the car-proven NBT-AAD PluginApp (Supra-iDrive6-Android-Auto, rhmiapp/nbt_aad_app.cpp):
//   * PluginApp vtable: 34 slots after the address point; 21 onError, 22 onInit, 23 onFinalize.
//     handleMessage(0xC0000000 = action) calls slot 16 invokeCommand(actionId, Parcel) AND slot 27
//     invokeCommandWithMap(actionId, Map<unsigned, Variant>) -> both handler kinds fire for every raAction.
//   * Variant = one pointer to an IObjectContainer { vtable: ~D1, ~D0, int type() const, clone() const; T value }.
//     type(): 0x22 = BasicString<char> (Variant::is<BasicString<char>> compares against 34), 0x28 = TextID,
//     0x2b = ImageID. Variant::as<BasicString<char>&>() returns container + 4.
//   * Map<unsigned, Variant> is MFC-CMap shaped: { assoc** table; u32 hashSize; u32 count; assoc* free;
//     block* blocks; u32 blockSize }, assoc = { assoc* next; u32 hash; u32 key; Variant value },
//     hash bucket = ror(key, 4) % hashSize. operator[] inserts, so const lookups walk the buckets by hand.
//   * BasicString<T> is MFC-CString shaped: one pointer to the characters; 12 bytes before them sit
//     { int refs; int length; int alloc }. Empty strings point into a shared static nil buffer.
#ifndef ID6TERM_BMW_RHMI_HPP
#define ID6TERM_BMW_RHMI_HPP

#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

namespace bmw {

class IDispatcher;
class Bundle;
class Runnable;              // vtable: ~D1, ~D0, void run()

class Parcel {
public:
    explicit Parcel(bool value);                 // _ZN3bmw6ParcelC1Eb
    ~Parcel();                                   // _ZN3bmw6ParcelD1Ev
    void readBool(bool& value) const;            // _ZNK3bmw6Parcel8readBoolERb
    unsigned size() const;                       // _ZNK3bmw6Parcel4sizeEv
    unsigned dataAvail() const;                  // _ZNK3bmw6Parcel9dataAvailEv
    const unsigned char* getRawData() const;     // _ZNK3bmw6Parcel10getRawDataEv
private:
    unsigned char m_storage[32];                 // >= sizeof(bmw::Parcel) (0x14): a Vector<unsigned char> + position
};

enum RHMIInterfaceVersion { RHMI_INTERFACE_VERSION_1 = 1 };

template <typename T> class BasicString {
public:
    BasicString(const T* s);                     // _ZN3bmw11BasicStringIcEC1EPKc / ...ItEC1EPKt
    BasicString(const BasicString& other);
    ~BasicString();
    const T* raw() const { return m_data; }
    // The CString header in front of the characters (valid for every live string, including the nil buffer).
    int length() const { return reinterpret_cast<const int*>(m_data)[-2]; }
private:
    T* m_data;
};

class TextID {
public:
    explicit TextID(unsigned id);                // _ZN3bmw6TextIDC1Ej -> { id, type 0x28 }
private:
    unsigned m_id;
    unsigned m_type;
};

class ImageID {
public:
    explicit ImageID(unsigned id);               // _ZN3bmw7ImageIDC1Ej -> { id, type 0x2b }
private:
    unsigned m_id;
    unsigned m_type;
};

class Variant {
public:
    class IObjectContainer {
    public:
        virtual ~IObjectContainer() {}
        virtual int type() const = 0;
        virtual IObjectContainer* clone() const = 0;
    };
    template <typename T> class ObjectContainer : public IObjectContainer {
    public:
        explicit ObjectContainer(const T& v) : m_value(v) {}
        virtual int type() const;
        virtual IObjectContainer* clone() const { return new ObjectContainer<T>(m_value); }
        const T& value() const { return m_value; }
    private:
        T m_value;
    };
    enum { kTypeString = 0x22, kTypeTextID = 0x28, kTypeImageID = 0x2b };
    ~Variant();                                  // _ZN3bmw7VariantD1Ev (deletes the container)
    IObjectContainer* m_obj;
};
template <> inline int Variant::ObjectContainer<TextID>::type() const { return kTypeTextID; }
template <> inline int Variant::ObjectContainer<ImageID>::type() const { return kTypeImageID; }
template <> inline int Variant::ObjectContainer< BasicString<char> >::type() const { return kTypeString; }

template <typename K, typename V> class Map {
public:
    struct Assoc {
        Assoc* next;
        unsigned hash;
        K key;
        V value;
    };
    Map() : m_table(0), m_hashSize(17), m_count(0), m_free(0), m_blocks(0), m_blockSize(10) {}
    ~Map();                                      // _ZN3bmw3MapIjNS_7VariantEED1Ev
    V& operator[](const K& key);                 // _ZN3bmw3MapIjNS_7VariantEEixERKj (inserts when missing)
    unsigned count() const { return m_count; }
    // Read-only lookup that walks every bucket (no dependence on the hash function); NULL when absent.
    const V* find(const K& key) const {
        if (!m_table) return 0;
        for (unsigned b = 0; b < m_hashSize; ++b)
            for (const Assoc* a = m_table[b]; a; a = a->next)
                if (a->key == key) return &a->value;
        return 0;
    }
    template <typename F> void forEach(F& f) const {
        if (!m_table) return;
        for (unsigned b = 0; b < m_hashSize; ++b)
            for (const Assoc* a = m_table[b]; a; a = a->next) f(a->key, a->value);
    }
private:
    Assoc** m_table;
    unsigned m_hashSize, m_count;
    void* m_free;
    void* m_blocks;
    unsigned m_blockSize;
};

class DataTableBase {
public:
    DataTableBase(Map<unsigned, Variant>* cells, unsigned columns);   // _ZN3bmw13DataTableBaseC1EPNS_3MapIjNS_7VariantEEEj
private:
    unsigned char m_storage[16];                 // 8 used
};

template <typename R, typename A1, typename A2, typename A3, typename A4> struct Delegate {
    void* object;
    R (*stub)(void* object, A1 a1);
};
typedef Delegate<bool, const Parcel&, void, void, void> ActionDelegate;
typedef Delegate<bool, const Map<unsigned, Variant>&, void, void, void> MapDelegate;
typedef Delegate<bool, const BasicString<unsigned short>&, void, void, void> CdsDelegate;

const size_t kPluginAppStorage = 0x200;          // SecurityMenu's own members start at 0x160

class PluginApp {
public:
    PluginApp(IDispatcher& dispatcher, BasicString<char> name, unsigned major, unsigned minor,
              RHMIInterfaceVersion rhmi, const Bundle& params, const Bundle& config);
    // raAction handler (Parcel form) and the same for notifications of one of our hmiStates (type 1 = focus,
    // 11 = visible; the Parcel carries one bool).
    void addHandler(unsigned actionId, const ActionDelegate& handler);
    void addHandler(unsigned id, unsigned type, const ActionDelegate& handler);
    // raAction handler (Map form): the action's arguments as { argId -> Variant }, e.g. { 8 -> letter } from the
    // speller, { 1 -> list index } from a list / suggestion row.
    void addHandlerWithMap(unsigned actionId, const MapDelegate& handler);
    void triggerEvent(unsigned eventId);                            // one of our XML <events>
    void post(Runnable* runnable, unsigned delayMs);                // runnable->run() on the app thread
    void updateModel(unsigned modelId, const TextID& text);         // textIdModel
    void updateModel(unsigned modelId, const ImageID& image);       // imageIdModel
    void updateModel(unsigned modelId, bool value);                 // raBoolModel
    void updateModel(unsigned modelId, const char* text);           // raDataModel (plain / Richtext label)
    void updateModel(unsigned modelId, const BasicString<char>& text);
    void updateModel(unsigned modelId, const DataTableBase& table); // raListModel
    // Component property: 1 enabled, 2 selectable, 3 visible, 8 LABEL_WAITINGANIMATION (busy ring).
    void setProperty(unsigned componentId, unsigned propertyId, const Parcel& value);
    bool onInit();                                                  // base implementations
    void onFinalize();
private:
    unsigned char m_storage[kPluginAppStorage];
};

// ---- helpers over the ABI above (header-only, no BMW symbols needed beyond the declarations) --------------------

enum { kSlotOnError = 21, kSlotOnInit = 22, kSlotOnFinalize = 23, kVtblSlots = 34 };

// Describes what an argument Variant holds, for the log and for the readers below.
struct VariantView {
    int type;                       // container->type(), -1 for an empty Variant
    const void* payload;            // container + 4 (the T value), NULL for an empty Variant
};

inline VariantView viewVariant(const Variant& v) {
    VariantView r;
    r.type = -1;
    r.payload = 0;
    if (!v.m_obj) return r;
    r.type = v.m_obj->type();
    r.payload = reinterpret_cast<const unsigned char*>(v.m_obj) + sizeof(void*);
    return r;
}

// Can n bytes at p be read without faulting? write() into a pipe copies from our address space and reports
// EFAULT for an unmapped source instead of killing the process (QNX and Linux alike). Used before the string
// heuristics below touch memory that an argument of an unknown Variant type merely *might* point to - an
// integer argument reinterpreted as a pointer must not take the HMI down. App thread only (one static pipe).
inline bool probeReadable(const void* p, size_t n) {
    static int fds[2] = { -1, -1 };
    if (!p || n == 0) return false;
    if (n > 4096) n = 4096;                                        // <= PIPE_BUF: the non-blocking write is atomic
    if ((size_t)p < 4096 || ((size_t)p & 3) != 0) return false;   // null page / unaligned: never a CString buffer
    if (fds[0] < 0) {
        if (pipe(fds) != 0) { fds[0] = fds[1] = -1; return false; }
        fcntl(fds[0], F_SETFD, FD_CLOEXEC);
        fcntl(fds[1], F_SETFD, FD_CLOEXEC);
        fcntl(fds[1], F_SETFL, fcntl(fds[1], F_GETFL) | O_NONBLOCK);   // never block the app thread
    }
    ssize_t w = write(fds[1], p, n);
    if (w > 0) {
        unsigned char sink[4096];
        size_t left = (size_t)w;
        while (left) {
            ssize_t r = read(fds[0], sink, left < sizeof(sink) ? left : sizeof(sink));
            if (r <= 0) break;
            left -= (size_t)r;
        }
    }
    return w == (ssize_t)n;
}

// Does p look like the character data of a bmw::BasicString (CString header right in front of it)?
// An address-plausibility check only; it is what lets an argument of an unknown Variant type code be read as
// text without dereferencing garbage.
inline bool looksLikeStringData(const void* p, int* lengthOut) {
    if (!p) return false;
    const int* hdr = reinterpret_cast<const int*>(p) - 3;      // refs, length, alloc
    if (!probeReadable(hdr, 12)) return false;
    int refs = hdr[0], len = hdr[1], alloc = hdr[2];
    if (len < 0 || len > 4096 || alloc < len || alloc > 65536) return false;
    if (refs < -1 || refs > 100000) return false;                // nil buffer has refs -1 .. small counts otherwise
    if (!probeReadable(p, (size_t)len * 2 + 2)) return false;    // the wider of the two possible layouts
    if (lengthOut) *lengthOut = len;
    return true;
}

// Copies a string argument (narrow BasicString<char>, or a wide BasicString<unsigned short> of an unknown type
// code, detected from the CString header + character layout) into out as ASCII/UTF-8-ish bytes. Returns false if
// the Variant is not a string.
inline bool variantToString(const Variant& v, char* out, size_t n) {
    if (!n) return false;
    out[0] = 0;
    VariantView vv = viewVariant(v);
    if (!vv.payload) return false;
    const void* data = *reinterpret_cast<void* const*>(vv.payload);   // BasicString::m_data
    int len = 0;
    if (!looksLikeStringData(data, &len)) return false;
    bool wide;
    if (vv.type == Variant::kTypeString) wide = false;
    else if (len == 0) wide = false;
    else if (len == 1) wide = reinterpret_cast<const unsigned char*>(data)[1] == 0 &&
                              reinterpret_cast<const unsigned char*>(data)[2] == 0 &&
                              reinterpret_cast<const unsigned char*>(data)[3] == 0;
    else wide = reinterpret_cast<const unsigned char*>(data)[1] == 0;   // "ab" wide = 61 00 62 00, narrow = 61 62
    size_t j = 0;
    if (!wide) {
        const char* s = reinterpret_cast<const char*>(data);
        for (int i = 0; i < len && s[i] && j + 1 < n; ++i) out[j++] = s[i];
    } else {
        const unsigned short* s = reinterpret_cast<const unsigned short*>(data);
        for (int i = 0; i < len && s[i] && j + 1 < n; ++i) {
            unsigned c = s[i];
            if (c < 0x80) out[j++] = (char)c;
            else if (c < 0x800 && j + 2 < n) { out[j++] = (char)(0xC0 | (c >> 6)); out[j++] = (char)(0x80 | (c & 0x3F)); }
            else if (j + 3 < n) { out[j++] = (char)(0xE0 | (c >> 12)); out[j++] = (char)(0x80 | ((c >> 6) & 0x3F)); out[j++] = (char)(0x80 | (c & 0x3F)); }
        }
    }
    out[j] = 0;
    return true;
}

// Reads an integer argument (list / suggestion index). The integer Variant type codes are not known, so the
// 32-bit payload is returned as is; callers range-check it against their list and fall back to the low byte /
// half-word if it is out of range (an 8- or 16-bit container leaves the upper bytes uninitialised).
inline bool variantToInt(const Variant& v, int* out) {
    VariantView vv = viewVariant(v);
    if (!vv.payload || vv.type == Variant::kTypeString) return false;
    *out = *reinterpret_cast<const int*>(vv.payload);
    return true;
}

} // namespace bmw

#endif // ID6TERM_BMW_RHMI_HPP
