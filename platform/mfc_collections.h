// platform/mfc_collections.h
// MFC collection class stubs for non-Windows platforms.
//
// Provides functionally-equivalent implementations of:
//   CTypedPtrList<CPtrList, T*>   — doubly-linked list of T*
//   CTypedPtrMap<CMapStringToPtr, CString, T*>  — CString-keyed map of T*
//   CTypedPtrArray<CPtrArray, T*> — resizable array of T*
//
// The MFC POSITION type is an opaque cursor; we implement it as a raw
// pointer into list node / map iterator storage.

#pragma once

#ifndef _WIN32

#include <list>
#include <map>
#include <vector>
#include <string>
#include <cassert>
#include "mfc_string.h"

// ── POSITION ─────────────────────────────────────────────────────────────
// MFC uses POSITION as an opaque non-null token for iteration.
// NULL POSITION means "end" or "not found".

typedef void* POSITION;

// ─────────────────────────────────────────────────────────────────────────
// CTypedPtrList  (backed by std::list<T*>)
// ─────────────────────────────────────────────────────────────────────────

// Tag types used as first template argument in MFC (ignored here)
struct CPtrList {};

template<typename _Base, typename TYPE>
class CTypedPtrList
{
public:
    typedef std::list<TYPE> ListType;
    typedef typename ListType::iterator       Iter;
    typedef typename ListType::const_iterator CIter;

    // Node wrapper — allocated on heap so we can hand out stable POSITION tokens
    struct Node {
        Iter it;   // iterator into list_
    };

    CTypedPtrList() = default;
    ~CTypedPtrList() { RemoveAll(); }

    // ── Move / copy ───────────────────────────────────────────────────────
    // The user-declared destructor suppresses the implicit move constructor
    // in C++11/17.  Declare it explicitly so containers that hold a
    // CTypedPtrList (e.g. CMUSHclientDoc) can be returned by value even when
    // NRVO does not fire.
    //
    // std::list move transfers ownership and keeps iterators valid (they now
    // point into the destination list), so the Node objects in nodes_ remain
    // correct after a move.
    CTypedPtrList(CTypedPtrList&& o) noexcept
        : list_(std::move(o.list_))
        , nodes_(std::move(o.nodes_))
    {}
    CTypedPtrList& operator=(CTypedPtrList&& o) noexcept {
        if (this != &o) {
            RemoveAll();
            list_  = std::move(o.list_);
            nodes_ = std::move(o.nodes_);
        }
        return *this;
    }
    // Copying is unsafe: the default copy would shallow-copy Node pointers
    // whose iterators point into the source's list_, not the copy's.
    CTypedPtrList(const CTypedPtrList&)            = delete;
    CTypedPtrList& operator=(const CTypedPtrList&) = delete;

    // ── Adding ────────────────────────────────────────────────────────────

    POSITION AddTail(TYPE val) {
        list_.push_back(val);
        Node* n = new Node{ std::prev(list_.end()) };
        nodes_.push_back(n);
        return (POSITION)n;
    }
    // Append all elements from another list (mirrors MFC CPtrList::AddTail(CPtrList*))
    void AddTail(CTypedPtrList* pOther) {
        if (!pOther) return;
        for (TYPE v : pOther->list_) AddTail(v);
    }
    POSITION AddHead(TYPE val) {
        list_.push_front(val);
        Node* n = new Node{ list_.begin() };
        nodes_.push_back(n);
        return (POSITION)n;
    }

    // ── Removal ───────────────────────────────────────────────────────────

    TYPE RemoveTail() {
        assert(!list_.empty());
        TYPE val = list_.back();
        RemoveNodeAt(std::prev(list_.end()));
        return val;
    }
    TYPE RemoveHead() {
        assert(!list_.empty());
        TYPE val = list_.front();
        RemoveNodeAt(list_.begin());
        return val;
    }
    void RemoveAt(POSITION pos) {
        Node* n = reinterpret_cast<Node*>(pos);
        RemoveNodeAt(n->it);
    }
    // std::list-compatible erase: removes element at `it`, returns next iterator.
    Iter erase(Iter it) {
        for (auto nit = nodes_.begin(); nit != nodes_.end(); ++nit) {
            if ((*nit)->it == it) {
                delete *nit;
                nodes_.erase(nit);
                break;
            }
        }
        return list_.erase(it);
    }
    void RemoveAll() {
        list_.clear();
        for (Node* n : nodes_) delete n;
        nodes_.clear();
    }

    // ── Access ────────────────────────────────────────────────────────────

    TYPE& GetHead()        { return list_.front(); }
    TYPE  GetHead() const  { return list_.front(); }
    TYPE& GetTail()        { return list_.back(); }
    TYPE  GetTail() const  { return list_.back(); }
    TYPE  GetAt(POSITION pos) const {
        return *reinterpret_cast<const Node*>(pos)->it;
    }
    TYPE& GetAt(POSITION pos) {
        return *reinterpret_cast<Node*>(pos)->it;
    }
    void  SetAt(POSITION pos, TYPE val) {
        *reinterpret_cast<Node*>(pos)->it = val;
    }

    // ── Iteration ─────────────────────────────────────────────────────────

    POSITION GetHeadPosition() const {
        if (list_.empty()) return nullptr;
        return nodeForIter(const_cast<ListType&>(list_).begin());
    }
    POSITION GetTailPosition() const {
        if (list_.empty()) return nullptr;
        return nodeForIter(std::prev(const_cast<ListType&>(list_).end()));
    }
    // GetNext: returns value at pos, then advances pos to next (or nullptr)
    TYPE GetNext(POSITION& pos) const {
        Node* n = reinterpret_cast<Node*>(pos);
        TYPE val = *n->it;
        Iter next = std::next(n->it);
        pos = (next == list_.end()) ? nullptr : nodeForIter(next);
        return val;
    }
    TYPE& GetNext(POSITION& pos) {
        Node* n = reinterpret_cast<Node*>(pos);
        TYPE& val = *n->it;
        Iter next = std::next(n->it);
        pos = (next == list_.end()) ? nullptr : nodeForIter(next);
        return val;
    }
    TYPE GetPrev(POSITION& pos) const {
        Node* n = reinterpret_cast<Node*>(pos);
        TYPE val = *n->it;
        pos = (n->it == list_.begin()) ? nullptr : nodeForIter(std::prev(n->it));
        return val;
    }

    // ── Query ─────────────────────────────────────────────────────────────

    int    GetCount() const  { return (int)list_.size(); }
    bool   IsEmpty()  const  { return list_.empty(); }
    // std::list-compatible aliases (used by test code and range-erase loops)
    size_t size()     const  { return list_.size(); }
    TYPE&  front()           { return list_.front(); }
    TYPE   front()    const  { return list_.front(); }
    TYPE&  back()            { return list_.back(); }
    TYPE   back()     const  { return list_.back(); }

    // ── Find ──────────────────────────────────────────────────────────────

    POSITION Find(TYPE val, POSITION start = nullptr) const {
        Iter begin = start ? reinterpret_cast<Node*>(start)->it
                           : const_cast<ListType&>(list_).begin();
        Iter end   = const_cast<ListType&>(list_).end();
        for (Iter it = begin; it != end; ++it)
            if (*it == val) return nodeForIter(it);
        return nullptr;
    }

    // ── Insert ────────────────────────────────────────────────────────────

    POSITION InsertBefore(POSITION pos, TYPE val) {
        Iter it = reinterpret_cast<Node*>(pos)->it;
        Iter ni = list_.insert(it, val);
        Node* n = new Node{ ni };
        nodes_.push_back(n);
        return (POSITION)n;
    }
    POSITION InsertAfter(POSITION pos, TYPE val) {
        Iter it = std::next(reinterpret_cast<Node*>(pos)->it);
        Iter ni = list_.insert(it, val);
        Node* n = new Node{ ni };
        nodes_.push_back(n);
        return (POSITION)n;
    }

    // Allow range-for by exposing begin/end
    Iter  begin()        { return list_.begin(); }
    Iter  end()          { return list_.end(); }
    CIter begin()  const { return list_.begin(); }
    CIter end()    const { return list_.end(); }

private:
    ListType        list_;
    std::vector<Node*> nodes_;  // owns all Node objects

    // Find or create a Node for an iterator.
    // We search nodes_ — this is O(n) but the collections are small.
    POSITION nodeForIter(Iter it) const {
        for (Node* n : nodes_)
            if (n->it == it) return (POSITION)n;
        // Should not happen — every live iterator has a Node
        assert(false);
        return nullptr;
    }

    void RemoveNodeAt(Iter it) {
        erase(it);  // delegate to public erase(); return value ignored
    }
};


// ─────────────────────────────────────────────────────────────────────────
// CTypedPtrMap  (backed by std::map<std::string, T*>)
// ─────────────────────────────────────────────────────────────────────────

struct CMapStringToPtr {};

template<typename _Base, typename KEY, typename VALUE>
class CTypedPtrMap
{
public:
    typedef std::map<std::string, VALUE> MapType;
    typedef typename MapType::iterator       Iter;
    typedef typename MapType::const_iterator CIter;

    // ── Insertion / update ────────────────────────────────────────────────

    void SetAt(const CString& key, VALUE val) {
        map_[key.StdString()] = val;
    }
    void SetAt(const std::string& key, VALUE val) {
        map_[key] = val;
    }
    void SetAt(const char* key, VALUE val) {
        map_[key ? key : ""] = val;
    }
    VALUE& operator[](const CString& key) {
        return map_[key.StdString()];
    }

    // ── Lookup ────────────────────────────────────────────────────────────

    bool Lookup(const CString& key, VALUE& val) const {
        auto it = map_.find(key.StdString());
        if (it == map_.end()) return false;
        val = it->second;
        return true;
    }
    bool Lookup(const char* key, VALUE& val) const {
        auto it = map_.find(std::string(key ? key : ""));
        if (it == map_.end()) return false;
        val = it->second;
        return true;
    }
    bool Lookup(const std::string& key, VALUE& val) const {
        auto it = map_.find(key);
        if (it == map_.end()) return false;
        val = it->second;
        return true;
    }

    // ── Removal ───────────────────────────────────────────────────────────

    bool RemoveKey(const CString& key) {
        return map_.erase(key.StdString()) > 0;
    }
    void RemoveAll() { map_.clear(); }

    // ── Query ─────────────────────────────────────────────────────────────

    int  GetCount() const  { return (int)map_.size(); }
    bool IsEmpty()  const  { return map_.empty(); }

    // MFC hash-table sizing hint — std::map doesn't need this; accept and ignore.
    void InitHashTable(UINT, BOOL = TRUE) {}

    // ── Iteration ─────────────────────────────────────────────────────────
    // MFC: GetStartPosition() returns first POSITION; GetNextAssoc() unpacks it.
    // We use map iterators wrapped in heap-allocated tokens.

    POSITION GetStartPosition() const {
        if (map_.empty()) return nullptr;
        auto* token = new IterToken{ const_cast<MapType&>(map_).begin(),
                                     const_cast<MapType*>(&map_) };
        return (POSITION)token;
    }
    void GetNextAssoc(POSITION& pos, CString& key, VALUE& val) const {
        auto* token = reinterpret_cast<IterToken*>(pos);
        key = CString(token->it->first);
        val = token->it->second;
        ++(token->it);
        if (token->it == token->map->end()) {
            delete token;
            pos = nullptr;
        } else {
            pos = (POSITION)token;
        }
    }

    // Allow range-for
    Iter  begin()       { return map_.begin(); }
    Iter  end()         { return map_.end(); }
    CIter begin() const { return map_.begin(); }
    CIter end()   const { return map_.end(); }

private:
    MapType map_;

    struct IterToken {
        Iter      it;
        MapType*  map;
    };
};


// ─────────────────────────────────────────────────────────────────────────
// CTypedPtrArray  (backed by std::vector<T*>)
// ─────────────────────────────────────────────────────────────────────────

struct CPtrArray {};

template<typename _Base, typename TYPE>
class CTypedPtrArray
{
public:
    // ── Add / resize ──────────────────────────────────────────────────────

    int  Add(TYPE val)          { vec_.push_back(val); return (int)vec_.size()-1; }
    void SetSize(int n, int = 0) { vec_.resize((size_t)n, nullptr); }
    void RemoveAll()            { vec_.clear(); }
    void SetAtGrow(int i, TYPE v) {
        if ((size_t)i >= vec_.size()) vec_.resize((size_t)i + 1, nullptr);
        vec_[i] = v;
    }

    // ── Access ────────────────────────────────────────────────────────────

    TYPE  GetAt(int i) const    { return vec_[i]; }
    TYPE& GetAt(int i)          { return vec_[i]; }
    TYPE  operator[](int i) const { return vec_[i]; }
    TYPE& operator[](int i)     { return vec_[i]; }
    void  SetAt(int i, TYPE v)  { vec_[i] = v; }

    // ── Query ─────────────────────────────────────────────────────────────

    int  GetSize()       const  { return (int)vec_.size(); }
    int  GetUpperBound() const  { return (int)vec_.size() - 1; }
    bool IsEmpty()       const  { return vec_.empty(); }

    void RemoveAt(int i, int count = 1) {
        vec_.erase(vec_.begin() + i, vec_.begin() + i + count);
    }

    // Raw pointer to first element — matches MFC's GetData() for qsort use.
    TYPE* GetData() { return vec_.empty() ? nullptr : vec_.data(); }

    // Allow range-for
    auto begin()       { return vec_.begin(); }
    auto end()         { return vec_.end(); }
    auto begin() const { return vec_.begin(); }
    auto end()   const { return vec_.end(); }

private:
    std::vector<TYPE> vec_;
};

#endif // !_WIN32
