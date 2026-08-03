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

// An INTRUSIVE doubly-linked list: the node carries the value, and POSITION is
// the node address.  That is what MFC's POSITION actually is, and it is the
// reason every cursor operation below is O(1).
//
// The previous implementation wrapped std::list and kept a separate vector of
// heap-allocated Node{iterator} objects, mapping an iterator back to its Node by
// LINEAR SEARCH ("this is O(n) but the collections are small" — which is false:
// CMUSHclientDoc::m_LineList is the entire scrollback).  Every GetNext() paid
// that search, so walking a list of N elements was O(N^2), and RemoveHead() —
// the head-trim in RemoveChunk — was O(N) per element removed.  At the default
// 5,000-line buffer a single full walk was ~12.5 million node comparisons; the
// display drain walks the line list on every received packet.
//
// Semantics preserved exactly: POSITION is an opaque non-null token, NULL means
// end/not-found, and a POSITION stays valid until its own element is removed.
template<typename _Base, typename TYPE>
class CTypedPtrList
{
public:
    struct Node {
        TYPE  val;
        Node* prev;
        Node* next;
    };

    // Forward iterator over the VALUES, so range-for and the std::list-shaped
    // helpers below keep working.  A null node is end().
    class iterator {
    public:
        iterator() : n_(nullptr) {}
        explicit iterator(Node* n) : n_(n) {}
        TYPE&     operator*()  const { return n_->val; }
        TYPE*     operator->() const { return &n_->val; }
        iterator& operator++()       { n_ = n_->next; return *this; }
        iterator  operator++(int)    { iterator t = *this; n_ = n_->next; return t; }
        bool operator==(const iterator& o) const { return n_ == o.n_; }
        bool operator!=(const iterator& o) const { return n_ != o.n_; }
        Node* node() const { return n_; }
    private:
        Node* n_;
    };
    typedef iterator Iter;
    typedef iterator CIter;

    CTypedPtrList() = default;
    ~CTypedPtrList() { RemoveAll(); }

    // ── Move / copy ───────────────────────────────────────────────────────
    // The user-declared destructor suppresses the implicit move constructor in
    // C++11/17.  Declare it explicitly so containers that hold a CTypedPtrList
    // (e.g. CMUSHclientDoc) can be returned by value even when NRVO does not
    // fire.  Nodes are heap-allocated, so moving is a pointer steal and every
    // outstanding POSITION stays valid.
    CTypedPtrList(CTypedPtrList&& o) noexcept
        : head_(o.head_), tail_(o.tail_), size_(o.size_)
    { o.head_ = o.tail_ = nullptr; o.size_ = 0; }
    CTypedPtrList& operator=(CTypedPtrList&& o) noexcept {
        if (this != &o) {
            RemoveAll();
            head_ = o.head_; tail_ = o.tail_; size_ = o.size_;
            o.head_ = o.tail_ = nullptr; o.size_ = 0;
        }
        return *this;
    }
    // Copying is unsafe: the default copy would shallow-copy the node pointers,
    // so two lists would own the same nodes.
    CTypedPtrList(const CTypedPtrList&)            = delete;
    CTypedPtrList& operator=(const CTypedPtrList&) = delete;

    // ── Adding ────────────────────────────────────────────────────────────

    POSITION AddTail(TYPE val) {
        Node* n = new Node{ val, tail_, nullptr };
        if (tail_) tail_->next = n; else head_ = n;
        tail_ = n;
        ++size_;
        return (POSITION)n;
    }
    // Append all elements from another list (mirrors MFC CPtrList::AddTail(CPtrList*))
    void AddTail(CTypedPtrList* pOther) {
        if (!pOther) return;
        for (Node* n = pOther->head_; n; n = n->next) AddTail(n->val);
    }
    POSITION AddHead(TYPE val) {
        Node* n = new Node{ val, nullptr, head_ };
        if (head_) head_->prev = n; else tail_ = n;
        head_ = n;
        ++size_;
        return (POSITION)n;
    }

    // ── Removal ───────────────────────────────────────────────────────────

    TYPE RemoveTail() {
        assert(tail_);
        TYPE val = tail_->val;
        Unlink(tail_);
        return val;
    }
    TYPE RemoveHead() {
        assert(head_);
        TYPE val = head_->val;
        Unlink(head_);
        return val;
    }
    void RemoveAt(POSITION pos) {
        assert(pos);
        Unlink(reinterpret_cast<Node*>(pos));
    }
    // std::list-compatible erase: removes the element at `it`, returns the
    // iterator that followed it.
    iterator erase(iterator it) {
        Node* n = it.node();
        assert(n);
        Node* nxt = n->next;
        Unlink(n);
        return iterator(nxt);
    }
    void RemoveAll() {
        for (Node* n = head_; n; ) { Node* nx = n->next; delete n; n = nx; }
        head_ = tail_ = nullptr;
        size_ = 0;
    }

    // ── Access ────────────────────────────────────────────────────────────

    TYPE& GetHead()        { assert(head_); return head_->val; }
    TYPE  GetHead() const  { assert(head_); return head_->val; }
    TYPE& GetTail()        { assert(tail_); return tail_->val; }
    TYPE  GetTail() const  { assert(tail_); return tail_->val; }
    TYPE  GetAt(POSITION pos) const {
        return reinterpret_cast<const Node*>(pos)->val;
    }
    TYPE& GetAt(POSITION pos) {
        return reinterpret_cast<Node*>(pos)->val;
    }
    void  SetAt(POSITION pos, TYPE val) {
        reinterpret_cast<Node*>(pos)->val = val;
    }

    // ── Iteration ─────────────────────────────────────────────────────────

    POSITION GetHeadPosition() const { return (POSITION)head_; }
    POSITION GetTailPosition() const { return (POSITION)tail_; }
    // GetNext: returns value at pos, then advances pos to next (or nullptr)
    TYPE GetNext(POSITION& pos) const {
        Node* n = reinterpret_cast<Node*>(pos);
        pos = (POSITION)n->next;
        return n->val;
    }
    TYPE& GetNext(POSITION& pos) {
        Node* n = reinterpret_cast<Node*>(pos);
        pos = (POSITION)n->next;
        return n->val;
    }
    TYPE GetPrev(POSITION& pos) const {
        Node* n = reinterpret_cast<Node*>(pos);
        pos = (POSITION)n->prev;
        return n->val;
    }
    TYPE& GetPrev(POSITION& pos) {
        Node* n = reinterpret_cast<Node*>(pos);
        pos = (POSITION)n->prev;
        return n->val;
    }

    // ── Query ─────────────────────────────────────────────────────────────

    int    GetCount() const  { return (int)size_; }
    bool   IsEmpty()  const  { return size_ == 0; }
    // std::list-compatible aliases (used by test code and range-erase loops)
    size_t size()     const  { return size_; }
    TYPE&  front()           { assert(head_); return head_->val; }
    TYPE   front()    const  { assert(head_); return head_->val; }
    TYPE&  back()            { assert(tail_); return tail_->val; }
    TYPE   back()     const  { assert(tail_); return tail_->val; }

    // ── Find ──────────────────────────────────────────────────────────────
    // Linear, exactly as MFC's CPtrList::Find is.

    POSITION Find(TYPE val, POSITION start = nullptr) const {
        for (Node* n = start ? reinterpret_cast<Node*>(start) : head_; n; n = n->next)
            if (n->val == val) return (POSITION)n;
        return nullptr;
    }

    // ── Insert ────────────────────────────────────────────────────────────

    POSITION InsertBefore(POSITION pos, TYPE val) {
        Node* at = reinterpret_cast<Node*>(pos);
        if (!at) return AddHead(val);
        Node* n = new Node{ val, at->prev, at };
        if (at->prev) at->prev->next = n; else head_ = n;
        at->prev = n;
        ++size_;
        return (POSITION)n;
    }
    POSITION InsertAfter(POSITION pos, TYPE val) {
        Node* at = reinterpret_cast<Node*>(pos);
        if (!at) return AddTail(val);
        Node* n = new Node{ val, at, at->next };
        if (at->next) at->next->prev = n; else tail_ = n;
        at->next = n;
        ++size_;
        return (POSITION)n;
    }

    // Allow range-for by exposing begin/end
    iterator begin()        { return iterator(head_); }
    iterator end()          { return iterator(nullptr); }
    iterator begin()  const { return iterator(head_); }
    iterator end()    const { return iterator(nullptr); }

private:
    Node*  head_ = nullptr;
    Node*  tail_ = nullptr;
    size_t size_ = 0;

    void Unlink(Node* n) {
        if (n->prev) n->prev->next = n->next; else head_ = n->next;
        if (n->next) n->next->prev = n->prev; else tail_ = n->prev;
        delete n;
        --size_;
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
