/*
 * router_engine.cpp - the metal router's engine (router.h)
 *
 * The lattice: one plane a metal, a cell every 'grid' µm. A line is a
 * walk of its centre over the cells, drawn as squares of its width at
 * each, so a straight run is a rectangle carried half a width past its
 * ends - the shapes drawn and the cells tested are the same thing.
 *
 * - What is already in the layout is cut into boxes (exact for Manhattan
 *   shapes, a little larger beside a slanted edge), and each box, grown by
 *   half a line and its clearance with rounded corners, is rasterised
 *   once into a bit a cell for each size of square ("type": a class's
 *   line, a via's landing on a metal). One bit then tells whether a
 *   square of that size may sit there.
 * - A net's own metal - its terminals and what is joined to them, through
 *   their own vias too - would block it as well, so near it each blocked
 *   cell is tested again exactly, for the net alone: only the part of the
 *   square outside its own metal must keep clear (a quadtree of the
 *   square down to a fraction of a µm). Keep-outs a net may go into are
 *   tested the same way and cost more instead of blocking.
 * - Other nets' lines are stamped into counts a cell, for each type: the
 *   cells where a square of that type would come too near them. A* a net,
 *   its costs grown where others are (PathFinder): the present count,
 *   more each pass, and a history that stays. A pass with no cell shared
 *   ends it; the best pass is kept, and nets still too near another are
 *   routed again around the others, or reported.
 * - Memory: a bit a cell a type for what is fixed, counts and history
 *   only in pages of 64 x 64 cells that something touched, a net's own
 *   exact cells in pages near its metal, a search's arrays its window's.
 *   Counted as they are made, and a run stops at Options::maxMemory.
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "router.h"
#include "router_verify.h"

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonDocument>

#include <algorithm>
#include <bit>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <numeric>
#include <thread>
#include <unordered_map>

namespace qucs_s::router {

int Result::routed() const
{
    int n = 0;
    for (const NetResult& r : nets) n += r.status == NetResult::Status::Routed || r.status == NetResult::Status::Kept;
    return n;
}

int Result::failed() const
{
    int n = 0;
    for (const NetResult& r : nets) n += r.status == NetResult::Status::Failed;
    return n;
}

namespace {

constexpr int Page = 64;            // cells along a page's side
constexpr float Infinite = std::numeric_limits<float>::max();
constexpr float WayInCost = 8;      // a step in a keep-out a net may only go into
constexpr int MostPlanes = 8;
constexpr double Crowded = 0.7;   // the share of tracks wanted that is warned of
// The heuristic a little more than the distance left: of two cells as good,
// the one nearer the target first. Without it A* opens most of the box
// between a net's ends; with it a route may be 0.1 % longer than the best.
constexpr float Tie = 1.0f + 1.0f / 1024;
// Nets searched at once: those along the order whose windows (and what
// their lines would stamp) do not meet, Batch at most, looked for among
// the next Ahead. Whatever the threads, so the routes are the same with one
// thread or many; and nets apart do not see each other anyway.
constexpr int Batch = 48;
constexpr int Ahead = 192;

// Moves: +x, -x, +y, -y, up a metal, down, a source, none yet.
enum Move : std::uint8_t { PlusX, MinusX, PlusY, MinusY, Up, Down, Source, NoMove };

// What a net may do at a cell, by what is fixed: as the bit says, or (near
// its own metal and its keep-outs) as tested for it alone.
enum Static : std::uint8_t { Unknown = 0, Blocked = 1, Free = 2, Costly = 3 };

std::atomic<qint64>* g_bytesSink = nullptr;

class Memory
{
public:
    void add(qint64 n)
    {
        const qint64 now = a_now.fetch_add(n) + n;
        qint64 peak = a_peak.load();
        while (now > peak && !a_peak.compare_exchange_weak(peak, now)) {}
    }
    void sub(qint64 n) { a_now.fetch_sub(n); }
    qint64 now() const { return a_now.load(); }
    qint64 peak() const { return a_peak.load(); }
    void resetPeak() { a_peak.store(a_now.load()); }

private:
    std::atomic<qint64> a_now{0}, a_peak{0};
};

class Bits
{
public:
    void init(std::size_t n, Memory* m)
    {
        a_w.assign((n + 63) / 64, 0);
        a_m = m;
        m->add(qint64(a_w.size() * 8));
    }
    ~Bits()
    {
        if (a_m) a_m->sub(qint64(a_w.size() * 8));
    }
    Bits() = default;
    Bits(Bits&& o) noexcept : a_w(std::move(o.a_w)), a_m(o.a_m) { o.a_m = nullptr; }
    Bits& operator=(Bits&& o) noexcept
    {
        if (a_m) a_m->sub(qint64(a_w.size() * 8));
        a_w = std::move(o.a_w);
        a_m = o.a_m;
        o.a_m = nullptr;
        return *this;
    }
    bool test(std::size_t i) const { return (a_w[i >> 6] >> (i & 63)) & 1u; }
    void set(std::size_t i) { a_w[i >> 6] |= std::uint64_t(1) << (i & 63); }
    void setRange(std::size_t a, std::size_t b)   // [a, b]
    {
        std::size_t wa = a >> 6, wb = b >> 6;
        const std::uint64_t ma = ~std::uint64_t(0) << (a & 63);
        const std::uint64_t mb = ~std::uint64_t(0) >> (63 - (b & 63));
        if (wa == wb) {
            a_w[wa] |= ma & mb;
            return;
        }
        a_w[wa] |= ma;
        for (std::size_t w = wa + 1; w < wb; ++w) a_w[w] = ~std::uint64_t(0);
        a_w[wb] |= mb;
    }
    bool empty() const { return a_w.empty(); }

private:
    std::vector<std::uint64_t> a_w;
    Memory* a_m = nullptr;
};

// A map of cells allocated in pages as something is written there; a
// read of a page never written is 0.
template<class T> class Paged
{
public:
    void init(int nx, int ny, Memory* m)
    {
        clear();
        a_px = (nx + Page - 1) / Page;
        a_py = (ny + Page - 1) / Page;
        a_nx = nx;
        a_pages.assign(std::size_t(a_px) * std::size_t(a_py), nullptr);
        a_m = m;
        a_m->add(qint64(a_pages.size() * sizeof(T*)));
    }
    ~Paged()
    {
        clear();
        if (a_m) a_m->sub(qint64(a_pages.size() * sizeof(T*)));
    }
    Paged() = default;
    Paged(const Paged&) = delete;
    Paged& operator=(const Paged&) = delete;
    Paged(Paged&& o) noexcept
        : a_pages(std::move(o.a_pages)), a_px(o.a_px), a_py(o.a_py), a_nx(o.a_nx), a_m(o.a_m), a_allocated(o.a_allocated)
    {
        o.a_m = nullptr;
        o.a_allocated = 0;
    }
    void clear()
    {
        for (T*& p : a_pages)
            if (p) {
                delete[] p;
                p = nullptr;
                a_m->sub(qint64(sizeof(T)) * Page * Page);
            }
        a_allocated = 0;
    }
    T get(std::size_t c) const
    {
        const int i = int(c % std::size_t(a_nx)), j = int(c / std::size_t(a_nx));
        const T* p = a_pages[std::size_t(j / Page) * std::size_t(a_px) + std::size_t(i / Page)];
        return p ? p[(j % Page) * Page + (i % Page)] : T(0);
    }
    T get(int i, int j) const
    {
        const T* p = a_pages[std::size_t(j / Page) * std::size_t(a_px) + std::size_t(i / Page)];
        return p ? p[(j % Page) * Page + (i % Page)] : T(0);
    }
    T& ref(int i, int j)
    {
        T*& p = a_pages[std::size_t(j / Page) * std::size_t(a_px) + std::size_t(i / Page)];
        if (!p) {
            p = new T[Page * Page]();
            ++a_allocated;
            a_m->add(qint64(sizeof(T)) * Page * Page);
        }
        return p[(j % Page) * Page + (i % Page)];
    }
    int pages() const { return a_allocated; }

private:
    std::vector<T*> a_pages;
    int a_px = 0, a_py = 0, a_nx = 1;
    Memory* a_m = nullptr;
    int a_allocated = 0;
};

// A few threads kept for a run: run(n, f) calls f(i, worker) for i < n.
class Pool
{
public:
    explicit Pool(int threads)
    {
        for (int t = 1; t < threads; ++t) a_threads.emplace_back([this, t] { loop(t); });
    }
    ~Pool()
    {
        {
            std::lock_guard<std::mutex> lock(a_mutex);
            a_quit = true;
        }
        a_wake.notify_all();
        for (std::thread& t : a_threads) t.join();
    }
    int size() const { return int(a_threads.size()) + 1; }
    void run(int n, const std::function<void(int, int)>& f)
    {
        if (n <= 0) return;
        if (a_threads.empty() || n == 1) {
            for (int i = 0; i < n; ++i) f(i, 0);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(a_mutex);
            a_job = &f;
            a_count = n;
            a_next = 0;
            a_done = 0;
            ++a_generation;
        }
        a_wake.notify_all();
        work(0);
        std::unique_lock<std::mutex> lock(a_mutex);
        a_finished.wait(lock, [&] { return a_done == a_count; });
        a_job = nullptr;
    }

private:
    void work(int worker)
    {
        for (;;) {
            int i;
            const std::function<void(int, int)>* job;
            {
                std::lock_guard<std::mutex> lock(a_mutex);
                if (!a_job || a_next >= a_count) return;
                i = a_next++;
                job = a_job;
            }
            (*job)(i, worker);
            std::lock_guard<std::mutex> lock(a_mutex);
            if (++a_done == a_count) a_finished.notify_all();
        }
    }
    void loop(int worker)
    {
        quint64 seen = 0;
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(a_mutex);
                a_wake.wait(lock, [&] { return a_quit || (a_generation != seen && a_job); });
                if (a_quit) return;
                seen = a_generation;
            }
            work(worker);
        }
    }
    std::vector<std::thread> a_threads;
    std::mutex a_mutex;
    std::condition_variable a_wake, a_finished;
    const std::function<void(int, int)>* a_job = nullptr;
    int a_count = 0, a_next = 0, a_done = 0;
    quint64 a_generation = 0;
    bool a_quit = false;
};

// A radix heap of (key, state): keys popped never fall, so a push costs
// nothing and a pop moves each entry at most 64 times in all. A* pops its
// f in order (a key below the last popped is taken as that one: the
// heuristic's small excess can make f fall by less than a step). Equal
// keys come out last pushed first, deepest first.
class RadixHeap
{
public:
    static constexpr double Scale = 256;   // key units a lattice step
    void push(double f, std::uint32_t s)
    {
        std::uint64_t k = f >= 1e15 ? std::uint64_t(1e15 * Scale) : std::uint64_t(f * Scale);
        if (k < a_last) k = a_last;
        a_b[bucketOf(k)].push_back({k, s});
        ++a_n;
    }
    bool empty() const { return a_n == 0; }
    /// The least key's entry; its key as f in \a f.
    std::uint32_t pop(double* f)
    {
        if (a_b[0].empty()) {
            int i = 1;
            while (a_b[i].empty()) ++i;
            std::uint64_t least = std::numeric_limits<std::uint64_t>::max();
            for (const auto& e : a_b[i]) least = std::min(least, e.first);
            a_last = least;
            for (const auto& e : a_b[i]) a_b[bucketOf(e.first)].push_back(e);
            a_b[i].clear();
        }
        const auto e = a_b[0].back();
        a_b[0].pop_back();
        --a_n;
        *f = double(e.first) / Scale;
        return e.second;
    }
    void clear()
    {
        for (auto& b : a_b) b.clear();
        a_last = 0;
        a_n = 0;
    }
    std::size_t capacityBytes() const
    {
        std::size_t n = 0;
        for (const auto& b : a_b) n += b.capacity() * sizeof(b[0]);
        return n;
    }

private:
    int bucketOf(std::uint64_t k) const { return k == a_last ? 0 : 64 - std::countl_zero(k ^ a_last); }
    std::vector<std::pair<std::uint64_t, std::uint32_t>> a_b[65];
    std::uint64_t a_last = 0;
    std::size_t a_n = 0;
};

// What a static box is.
enum class Kind : std::uint8_t { Metal, Terminal, Obstacle, KeepOut };

struct StaticBox {
    Box box;
    Coord clearance = 0;
    Kind kind = Kind::Metal;
    int owner = -1;             // the net whose metal it is
    int source = -1;            // Problem::metal's, obstacles' or keepOuts' index
};

// A size of square tested on the lattice: a class's line, a landing.
struct Type {
    Coord half = 0;
    Coord spacing = 0;          // its net's spacing to other lines
    bool operator==(const Type& o) const { return half == o.half && spacing == o.spacing; }
};

quint64 packState(int plane, std::size_t cell) { return (quint64(plane) << 40) | quint64(cell); }
int planeOf(quint64 s) { return int(s >> 40); }
std::size_t cellOf(quint64 s) { return std::size_t(s & ((quint64(1) << 40) - 1)); }

quint64 packLattice(int plane, Coord gx, Coord gy)
{
    return (quint64(plane & 0xF) << 60) | (quint64(gx + (Coord(1) << 29)) << 30) | quint64(gy + (Coord(1) << 29));
}

QString micro(double um)
{
    return QString::number(um, 'f', um == std::floor(um) ? 0 : (std::abs(um * 10 - std::round(um * 10)) < 1e-9 ? 1 : 3));
}

} // namespace

// ----------------------------------------------------------------------

struct Router::Impl {
    Problem p;
    QString error;
    Memory memory;
    qint64 estimate = 0;

    Coord pitch = 1;
    Box area;
    Coord ox = 0, oy = 0;
    int nx = 0, ny = 0, planes = 0;
    std::size_t cells = 0;

    std::vector<Type> types;
    std::vector<std::vector<int>> typesOnPlane;   // which types a plane has
    std::vector<int> classLine;                   // a class's line type
    std::vector<std::vector<int>> classLandLower, classLandUpper;   // [class][via]

    std::vector<std::vector<StaticBox>> statics;  // [plane]
    std::vector<BoxIndex> staticIndex;
    std::vector<std::vector<Box>> ownInner;       // [plane]: inner boxes of owned metal
    std::vector<std::vector<int>> ownInnerOwner;
    std::vector<std::vector<int>> ownInnerPiece;  // [plane]: the piece of metal each is part of
    std::vector<BoxIndex> ownIndex;
    std::vector<std::vector<int>> keepOutNets;    // [keep-out]: nets it lets in
    Coord maxClearance = 0;

    std::vector<std::vector<Bits>> base;          // [type][plane]
    std::vector<std::vector<Paged<std::uint16_t>>> use;   // [type][plane]
    std::vector<Paged<float>> history;            // [plane]

    struct Zone {
        int type = 0, plane = 0;
        std::unordered_map<std::uint32_t, std::vector<std::uint8_t>> pages;
    };
    struct Leg {
        std::vector<std::pair<quint64, float>> cells;   // state and tail cost (steps)
        Pt point;               // where the tail goes
        bool tail = true;
        Box shape;              // the terminal's shape, for the heuristic of Anywhere
        int joined = 0;
        std::vector<int> metals;
        std::vector<int> pieces;   // the pieces of metal it is on
        QString warning;           // what route_setup should say of it
    };
    struct NetState {
        int cls = 0;
        int lineType = 0;
        Box window;             // cells: i0, j0, i1, j1
        int grow = 0;           // contested passes: its window doubled as often
        std::vector<Zone> zones;
        std::vector<Leg> terminals;
        std::vector<quint64> path;          // the states it uses
        std::vector<std::pair<int, Box>> metal;   // what it draws: plane and box
        NetResult result;
        bool routed = false, failed = false, kept = false, contested = false;
        quint64 key = 0;
        Bits guideBand;         // [window cell], when it has a guide
        bool hasGuide = false;
    };
    std::vector<NetState> nets;
    QVector<TerminalReading> readings;
    QVector<Crowding> crowding;
    std::atomic<quint64> searched{0};
    bool released = false;   // releaseMaps(): prepared again before a run

    // Prepare
    bool prepare();
    bool buildTypes();
    bool buildStatics();
    void rasterise();
    void buildZone(int net, int type, int plane, Zone* z);
    void buildTerminals(int net);
    void estimateCrowding();
    quint64 key(int net) const;

    // Cells
    /// The cells a net is searched in: its window, grown each pass it was
    /// contested (a way round may lie outside it), to the whole area.
    Box windowOf(int n) const
    {
        const NetState& st = nets[std::size_t(n)];
        if (st.grow <= 0) return st.window;
        const Coord extent = std::max(st.window.width(), st.window.height()) + 1;
        const Coord g = st.grow >= 20 ? Coord(nx + ny) : extent * ((Coord(1) << st.grow) - 1) / 2;
        const Box w = st.window.grown(g);
        return {std::max<Coord>(0, w.x0), std::max<Coord>(0, w.y0), std::min<Coord>(nx - 1, w.x1), std::min<Coord>(ny - 1, w.y1)};
    }
    std::size_t cellAt(int i, int j) const { return std::size_t(j) * std::size_t(nx) + std::size_t(i); }
    Pt pointOf(std::size_t c) const { return {ox + Coord(c % std::size_t(nx)) * pitch, oy + Coord(c / std::size_t(nx)) * pitch}; }
    template<class F> void raster(const Box& b, Coord half, Coord s, F&& row) const;

    // Search
    struct Buffers;
    std::uint8_t staticAt(const NetState& n, int type, int plane, std::size_t c, const std::vector<std::uint8_t*>* table) const;
    bool exactClear(int net, int type, int plane, Pt centre, bool* costly) const;
    bool search(int net, const std::vector<std::pair<quint64, float>>& sources, const Leg& target, const Box& window, bool strict, double present,
                Buffers& b, std::vector<quint64>* path, const Bits* band, int bandTarget, Pt* nearest) const;
    bool route(int net, bool strict, double present, Buffers& b, bool fullArea);
    void draw(int net);
    void stamp(int net, int sign);
    void stampBoxes(const std::vector<std::pair<int, Box>>& metal, int lineType, int sign);
    /// Calls f(type, plane, row, i0, i1) for each row of cells the metal's
    /// stamps cover, type by type.
    template<class F> void visitStamps(const std::vector<std::pair<int, Box>>& metal, int lineType, F&& f) const
    {
        const Coord own = types[std::size_t(lineType)].spacing;
        for (const auto& [plane, box] : metal)
            for (int k : typesOnPlane[std::size_t(plane)]) {
                const Coord s = std::max(own, types[std::size_t(k)].spacing);
                raster(box, types[std::size_t(k)].half, s, [&](int j, int i0, int i1) { f(k, plane, j, i0, i1); });
            }
    }
    std::vector<Bits> cutBlocked;   // [via]: a new cut too near one already there
    int viaGap = 0;                 // cells within which a net's vias are too near each other
    bool conflicted(int net);
    bool stillClear(int net) const;
    /// Whether a line of the net from \a from to \a to - straight, or an L
    /// either way round - keeps clear of what is fixed (its own metal aside);
    /// the corner taken in \a corner.
    bool tailClear(int net, int plane, Pt from, Pt to, Pt* corner = nullptr, bool horizontalFirst = true) const;
    bool runClear(int net, int plane, Pt a, Pt b) const;
    QString diagnose(int net, Pt* where);
    Result runAll(const QStringList& which, const QVector<Kept>& kept, const Progress& progress);
};

namespace {
struct Window {
    int i0, j0, i1, j1;
};
} // namespace

struct Router::Impl::Buffers {
    std::vector<float> g;
    std::vector<std::uint8_t> from;
    std::vector<std::uint8_t> closed;
    std::vector<std::uint32_t> touched;
    RadixHeap heap;
    std::vector<std::uint8_t*> table;   // the net's zone pages, by page, for its type and plane
    std::size_t size = 0;
    qint64 extra = 0;           // the heap's and the touched list's, as last counted
    Memory* memory = nullptr;
    ~Buffers()
    {
        if (memory) memory->sub(qint64(size) * 6 + extra);
    }
    /// The heap and the list of cells touched counted as they now are.
    void account()
    {
        if (!memory) return;
        const qint64 now = qint64(heap.capacityBytes() + touched.capacity() * sizeof(std::uint32_t));
        memory->add(now - extra);
        extra = now;
    }
    void ensure(std::size_t n, Memory* m)
    {
        if (n <= size) return;
        memory = m;
        m->add(qint64(n - size) * 6);
        size = n;
        g.assign(n, Infinite);
        from.assign(n, NoMove);
        closed.assign(n, 0);
        touched.clear();
    }
    void reset()
    {
        if (touched.size() > size / 4) {
            std::fill(g.begin(), g.end(), Infinite);
            std::fill(from.begin(), from.end(), NoMove);
            std::fill(closed.begin(), closed.end(), 0);
        } else {
            for (std::uint32_t s : touched) {
                g[s] = Infinite;
                from[s] = NoMove;
                closed[s] = 0;
            }
        }
        touched.clear();
        heap.clear();
    }
};

// Calls row(j, i0, i1) for every row of cells whose centre a square of half
// side 'half' there would bring closer than 's' to b (s = 0: overlapping it).
template<class F> void Router::Impl::raster(const Box& b, Coord half, Coord s, F&& row) const
{
    const Coord gx0 = b.x0 - half, gx1 = b.x1 + half, gy0 = b.y0 - half, gy1 = b.y1 + half;
    const auto lo = [&](Coord v) { return Coord(std::floor(double(v - oy) / double(pitch))); };
    int j0 = int(std::max<Coord>(0, lo(gy0 - s)));
    int j1 = int(std::min<Coord>(ny - 1, lo(gy1 + s) + 1));
    const double s2 = double(s) * double(s);
    for (int j = j0; j <= j1; ++j) {
        const Coord cy = oy + Coord(j) * pitch;
        Coord xa, xb;   // forbidden centres: (xa, xb), open
        if (s <= 0) {
            if (!(cy > gy0 && cy < gy1)) continue;
            xa = gx0;
            xb = gx1;
        } else {
            const Coord dy = std::max<Coord>({0, gy0 - cy, cy - gy1});
            if (double(dy) * double(dy) >= s2) continue;
            const double t = s2 - double(dy) * double(dy);
            // the largest whole dx with dx * dx < t, and the open interval
            // it gives: |overhang| <= r is forbidden.
            Coord r = Coord(std::ceil(std::sqrt(t))) + 1;
            while (r > 0 && double(r) * double(r) >= t) --r;
            xa = gx0 - r - 1;
            xb = gx1 + r + 1;
        }
        // cells with xa < cx < xb
        Coord i0 = Coord(std::floor(double(xa - ox) / double(pitch))) + 1;
        Coord i1 = Coord(std::ceil(double(xb - ox) / double(pitch))) - 1;
        while (ox + i0 * pitch <= xa) ++i0;
        while (i0 > 0 && ox + (i0 - 1) * pitch > xa) --i0;
        while (ox + i1 * pitch >= xb) --i1;
        while (i1 + 1 < nx && ox + (i1 + 1) * pitch < xb) ++i1;
        i0 = std::max<Coord>(0, i0);
        i1 = std::min<Coord>(nx - 1, i1);
        if (i0 <= i1) row(j, int(i0), int(i1));
    }
}

bool Router::Impl::buildTypes()
{
    const Technology& t = p.tech;
    types.clear();
    const auto typeOf = [&](Coord half, Coord spacing) {
        const Type k{half, spacing};
        for (int i = 0; i < int(types.size()); ++i)
            if (types[std::size_t(i)] == k) return i;
        types.push_back(k);
        return int(types.size()) - 1;
    };
    typesOnPlane.assign(std::size_t(planes), {});
    classLine.assign(std::size_t(t.classes.size()), 0);
    classLandLower.assign(std::size_t(t.classes.size()), std::vector<int>(std::size_t(t.vias.size()), 0));
    classLandUpper = classLandLower;
    const auto onPlane = [&](int type, int plane) {
        auto& v = typesOnPlane[std::size_t(plane)];
        if (std::find(v.begin(), v.end(), type) == v.end()) v.push_back(type);
    };
    for (int c = 0; c < t.classes.size(); ++c) {
        const NetClass& k = t.classes.at(c);
        const Coord s = p.units(std::max(k.spacing, t.sameLayer));
        const int line = typeOf(p.units(k.width) / 2, s);
        classLine[std::size_t(c)] = line;
        for (int l = 0; l < planes; ++l) onPlane(line, l);
        for (int v = 0; v < t.vias.size(); ++v) {
            const Via& via = t.vias.at(v);
            // A landing never narrower than the line that reaches it.
            const int lower = typeOf(std::max(p.units(via.landingLower) / 2, p.units(k.width) / 2), s);
            const int upper = typeOf(std::max(p.units(via.landingUpper) / 2, p.units(k.width) / 2), s);
            classLandLower[std::size_t(c)][std::size_t(v)] = lower;
            classLandUpper[std::size_t(c)][std::size_t(v)] = upper;
            onPlane(lower, via.lower);
            onPlane(upper, via.lower + 1);
        }
    }
    return true;
}

bool Router::Impl::prepare()
{
    error.clear();
    readings.clear();
    crowding.clear();
    nets.clear();
    const Technology& t = p.tech;
    planes = int(t.metals.size());
    if (planes < 1 || planes > MostPlanes) return error = QStringLiteral("The technology has no metals to route on."), false;
    if (t.classes.isEmpty()) return error = QStringLiteral("The technology has no classes of line."), false;
    if (p.unit <= 0 || !std::isfinite(p.unit)) return error = QStringLiteral("The layout's unit is not a length."), false;
    pitch = p.units(t.grid);
    if (pitch < 1) return error = QStringLiteral("The grid (%1 µm) is finer than the layout's unit.").arg(t.grid), false;
    if (p.nets.isEmpty()) return error = QStringLiteral("There are no nets to route."), false;

    // The area: the terminals' and guides' box grown by the margin, or the box given.
    Box box;
    for (const Net& n : std::as_const(p.nets)) {
        if (n.terminals.size() < 2) return error = QStringLiteral("Net %1 needs a start and a pad.").arg(n.name), false;
        if (n.netClass < 0 || n.netClass >= t.classes.size()) return error = QStringLiteral("Net %1 has no class.").arg(n.name), false;
        for (const Terminal& term : n.terminals) {
            box = box.united(Box{term.at.x, term.at.y, term.at.x, term.at.y});
            if (term.metal >= planes) return error = QStringLiteral("Net %1 names a metal the technology has not.").arg(n.name), false;
        }
        for (const Pt& g : n.guide) box = box.united(Box{g.x, g.y, g.x, g.y});
    }
    if (t.box.size() == 4) {
        area = {p.units(t.box[0]), p.units(t.box[1]), p.units(t.box[2]), p.units(t.box[3])};
        for (const Net& n : std::as_const(p.nets))
            for (const Terminal& term : n.terminals)
                if (!area.contains(term.at))
                    return error = QStringLiteral("Net %1 has a terminal at (%2, %3) outside the area the technology allows.")
                                       .arg(n.name, micro(p.um(term.at.x)), micro(p.um(term.at.y))),
                           false;
    } else {
        area = box.grown(std::max(p.units(t.margin), pitch * 4));
    }
    ox = Coord(std::floor(double(area.x0) / double(pitch))) * pitch;
    oy = Coord(std::floor(double(area.y0) / double(pitch))) * pitch;
    const double fx = double(area.x1 - ox) / double(pitch) + 1, fy = double(area.y1 - oy) / double(pitch) + 1;
    if (fx * fy * planes > 4e9)
        return error = QStringLiteral("The area is %1 x %2 µm: at a grid of %3 µm that is more than 4 000 million cells. Use a coarser grid, or a smaller area.")
                           .arg(micro(p.um(area.width())), micro(p.um(area.height())), micro(t.grid)),
               false;
    nx = int(fx);
    ny = int(fy);
    cells = std::size_t(nx) * std::size_t(ny);

    buildTypes();

    // What it will hold: the bits for what is fixed, the counts and history
    // where lines go (a third of the area at most, from the chips measured),
    // one search's arrays a thread over the whole area (its costs and moves,
    // and the cells touched and queued: 12 bytes a cell), a quarter more for
    // what grows from pass to pass - the black-box test of 7 November found
    // the use 15 to 25 % over the bare sum.
    const qint64 bits = qint64(cells / 8 + 8) * qint64(std::accumulate(typesOnPlane.begin(), typesOnPlane.end(), std::size_t(0),
                                                                     [](std::size_t a, const std::vector<int>& v) { return a + v.size(); }));
    const qint64 counts = qint64(cells) * planes * qint64(2 * types.size() + 4) / 3;
    const qint64 search = qint64(cells) * planes * 12;
    int threads = p.options.threads > 0 ? p.options.threads : int(std::max(1u, std::thread::hardware_concurrency()));
    const qint64 cap = p.options.maxMemory * 1024 * 1024;
    while (threads > 1 && (bits + counts + search * threads) * 5 / 4 > cap) --threads;
    p.options.threads = threads;
    estimate = (bits + counts + search * threads) * 5 / 4;
    if (estimate > cap)
        return error = QStringLiteral("Routing this would need about %1 MB, more than the %2 MB allowed (max_memory_mb): %3 x %4 cells on %5 metals. "
                                      "A coarser grid (%6 µm is a quarter of it) or a smaller area would do.")
                           .arg(estimate / (1024 * 1024))
                           .arg(p.options.maxMemory)
                           .arg(nx)
                           .arg(ny)
                           .arg(planes)
                           .arg(micro(t.grid * 2)),
               false;

    nets.resize(std::size_t(p.nets.size()));
    if (!buildStatics()) return false;
    rasterise();
    // New cuts keep their spacing from the cuts already there, and a net's
    // own vias from each other (within viaGap cells along its way).
    cutBlocked.clear();
    cutBlocked.resize(std::size_t(t.vias.size()));
    viaGap = 0;
    for (int v = 0; v < t.vias.size(); ++v) {
        const Via& via = t.vias.at(v);
        const Coord size = p.units(via.outline()), spacing = p.units(via.spacing);   // (an array: its outline)
        viaGap = std::max(viaGap, int((size + spacing + pitch - 1) / pitch));
        if (spacing <= 0) continue;
        bool any = false;
        for (const Shape& c : std::as_const(p.cuts)) any = any || c.layer == v;
        if (!any) continue;
        cutBlocked[std::size_t(v)].init(cells, &memory);
        for (const Shape& c : std::as_const(p.cuts))
            if (c.layer == v)
                raster(boundsOf(c.outline), size / 2, spacing,
                       [&](int j, int i0, int i1) { cutBlocked[std::size_t(v)].setRange(cellAt(i0, j), cellAt(i1, j)); });
    }

    use.clear();
    use.resize(types.size());
    for (std::size_t k = 0; k < types.size(); ++k) {
        use[k].resize(std::size_t(planes));
        for (int l = 0; l < planes; ++l)
            if (std::find(typesOnPlane[std::size_t(l)].begin(), typesOnPlane[std::size_t(l)].end(), int(k)) != typesOnPlane[std::size_t(l)].end())
                use[k][std::size_t(l)].init(nx, ny, &memory);
    }
    history.clear();
    history.resize(std::size_t(planes));
    for (int l = 0; l < planes; ++l) history[std::size_t(l)].init(nx, ny, &memory);

    // Each net: its window, its zones, its terminals' cells - nets apart from
    // each other, so in parallel.
    {
        Pool pool(p.options.threads);
        pool.run(int(nets.size()), [&](int n, int) { buildTerminals(n); });
    }
    for (int n = 0; n < int(nets.size()); ++n) {
        nets[std::size_t(n)].key = key(n);
        for (const Leg& leg : nets[std::size_t(n)].terminals) {
            TerminalReading r;
            r.net = p.nets.at(n).name;
            r.index = int(&leg - nets[std::size_t(n)].terminals.data());
            r.centre = leg.point;
            r.metals = QVector<int>(leg.metals.begin(), leg.metals.end());
            r.shape = leg.shape;
            r.joined = leg.joined;
            QStringList notes;
            if (!leg.warning.isEmpty()) notes << leg.warning;
            const Terminal& term = p.nets.at(n).terminals.at(r.index);
            for (const KeepOut& k : std::as_const(p.keepOuts))
                if (k.letThrough == KeepOut::LetThrough::None && inside(k.outline, term.at))
                    notes << QStringLiteral("it is inside the keep-out %1, which lets no net in: it cannot be left").arg(k.name);
            if (leg.cells.empty() && !p.nets.at(n).fixed) {
                const NetClass& k = t.classes.at(nets[std::size_t(n)].cls);
                const QString cure = term.mode == Terminal::Mode::Anywhere
                                         ? QStringLiteral("a narrower class, or more room round it")
                                         : QStringLiteral("a narrower class, the terminal's mode anywhere (the line ends where it meets the shape), or more room round it");
                notes << (leg.shape.empty()
                              ? QStringLiteral("no %1 µm line fits at this point clear of what is around it").arg(micro(k.width))
                              : QStringLiteral("no %1 µm line can leave this shape (%2 x %3 µm)%4 clear of what is around it: %5")
                                    .arg(micro(k.width), micro(p.um(leg.shape.width())), micro(p.um(leg.shape.height())),
                                         term.mode == Terminal::Mode::Anywhere ? QString() : QStringLiteral(", or reach its middle,"), cure));
            }
            r.note = notes.join(QStringLiteral("; "));
            readings.append(r);
        }
    }
    estimateCrowding();
    return true;
}

bool Router::Impl::buildStatics()
{
    const Technology& t = p.tech;
    statics.assign(std::size_t(planes), {});
    ownInner.assign(std::size_t(planes), {});
    ownInnerOwner.assign(std::size_t(planes), {});
    ownInnerPiece.assign(std::size_t(planes), {});
    const Coord slab = std::max<Coord>(1, pitch / 2);
    const Coord toMetal = p.units(t.toMetal), toTerminal = p.units(t.toTerminal);

    // Metal joined into pieces: boxes of a shape are one piece, shapes whose
    // boxes join are one, and a cut joins the metals over and under it.
    const int ns = int(p.metal.size());
    std::vector<int> parent(static_cast<std::size_t>(ns));
    std::iota(parent.begin(), parent.end(), 0);
    const std::function<int(int)> find = [&](int a) {
        while (parent[std::size_t(a)] != a) a = parent[std::size_t(a)] = parent[std::size_t(parent[std::size_t(a)])];
        return a;
    };
    const auto unite = [&](int a, int b) {
        a = find(a);
        b = find(b);
        if (a != b) parent[std::size_t(std::max(a, b))] = std::min(a, b);
    };
    std::vector<std::vector<Box>> outerOf(static_cast<std::size_t>(ns)), innerOf(static_cast<std::size_t>(ns));
    std::vector<std::vector<Box>> planeBoxes(static_cast<std::size_t>(planes));
    std::vector<std::vector<int>> planeShape(static_cast<std::size_t>(planes));
    for (int s = 0; s < ns; ++s) {
        const Shape& sh = p.metal.at(s);
        if (sh.layer < 0 || sh.layer >= planes) continue;
        boxesOf(sh.outline, slab, &outerOf[std::size_t(s)], &innerOf[std::size_t(s)]);
        for (const Box& b : outerOf[std::size_t(s)]) {
            planeBoxes[std::size_t(sh.layer)].push_back(b);
            planeShape[std::size_t(sh.layer)].push_back(s);
        }
    }
    std::vector<BoxIndex> metalIndex(static_cast<std::size_t>(planes));
    for (int l = 0; l < planes; ++l) {
        metalIndex[std::size_t(l)].build(planeBoxes[std::size_t(l)]);
        const auto& boxes = planeBoxes[std::size_t(l)];
        for (std::size_t i = 0; i < boxes.size(); ++i)
            metalIndex[std::size_t(l)].query(boxes[i], [&](int j) {
                if (std::size_t(j) > i && boxes[i].joins(boxes[std::size_t(j)])) unite(planeShape[std::size_t(l)][i], planeShape[std::size_t(l)][std::size_t(j)]);
                return true;
            });
    }
    for (const Shape& cut : std::as_const(p.cuts)) {
        if (cut.layer < 0 || cut.layer >= t.vias.size()) continue;
        const int lo = t.vias.at(cut.layer).lower;
        const Box cb = boundsOf(cut.outline);
        int below = -1, above = -1;
        metalIndex[std::size_t(lo)].query(cb, [&](int j) {
            if (planeBoxes[std::size_t(lo)][std::size_t(j)].overlaps(cb)) below = planeShape[std::size_t(lo)][std::size_t(j)];
            return below < 0;
        });
        metalIndex[std::size_t(lo + 1)].query(cb, [&](int j) {
            if (planeBoxes[std::size_t(lo + 1)][std::size_t(j)].overlaps(cb)) above = planeShape[std::size_t(lo + 1)][std::size_t(j)];
            return above < 0;
        });
        if (below >= 0 && above >= 0) unite(below, above);
    }

    // Which piece each terminal is on, and so which net owns it.
    std::vector<int> owner(std::size_t(ns), -1);       // by piece root
    std::vector<char> isPad(std::size_t(ns), 0);       // the shape a terminal is on
    nets.resize(std::size_t(p.nets.size()));
    for (int n = 0; n < p.nets.size(); ++n) {
        const Net& net = p.nets.at(n);
        NetState& st = nets[std::size_t(n)];
        st.cls = net.netClass;
        st.lineType = classLine[std::size_t(net.netClass)];
        st.terminals.assign(std::size_t(net.terminals.size()), Leg());
        for (int ti = 0; ti < net.terminals.size(); ++ti) {
            const Terminal& term = net.terminals.at(ti);
            Leg& leg = st.terminals[std::size_t(ti)];
            leg.point = term.at;
            const Box at{term.at.x, term.at.y, term.at.x, term.at.y};
            // The shapes under its point: on its metal when it names one, else
            // the lowest metal there. A terminal is that piece of metal, with
            // what is joined to it through cuts; metal over it with no cut is
            // not it (it is said, and left to the net).
            struct Under {
                int plane, shape, root;
            };
            std::vector<Under> under;
            for (int l = 0; l < planes; ++l) {
                if (term.metal >= 0 && term.metal != l) continue;
                metalIndex[std::size_t(l)].query(at, [&](int j) {
                    const int s = planeShape[std::size_t(l)][std::size_t(j)];
                    if (inside(p.metal.at(s).outline, term.at)) under.push_back({l, s, find(s)});
                    return true;
                });
            }
            const int chosen = under.empty() ? -1 : under.front().root;   // (planes in order: the lowest first)
            QStringList apart;
            for (const Under& u : under) {
                const int root = u.root;
                if (owner[std::size_t(root)] >= 0 && owner[std::size_t(root)] != n) {
                    error = QStringLiteral("Nets %1 and %2 have terminals on the same metal (at %3, %4 µm): they are one net already.")
                                .arg(p.nets.at(owner[std::size_t(root)]).name, net.name, micro(p.um(term.at.x)), micro(p.um(term.at.y)));
                    return false;
                }
                owner[std::size_t(root)] = n;
                if (root != chosen) {
                    if (!apart.contains(t.metals.at(u.plane).name)) apart << t.metals.at(u.plane).name;
                    continue;
                }
                if (isPad[std::size_t(u.shape)] == 0) isPad[std::size_t(u.shape)] = 1;
                if (std::find(leg.pieces.begin(), leg.pieces.end(), root) == leg.pieces.end()) leg.pieces.push_back(root);
                if (std::find(leg.metals.begin(), leg.metals.end(), u.plane) == leg.metals.end()) leg.metals.push_back(u.plane);
                leg.shape = leg.shape.united(boundsOf(p.metal.at(u.shape).outline));
            }
            if (!apart.isEmpty())
                leg.warning = QStringLiteral("%1 over this terminal is not joined to its %2 (no cut between them): the line ends on %2")
                                  .arg(apart.join(QStringLiteral(", ")), t.metals.at(under.front().plane).name);
            if (leg.metals.empty()) {
                if (term.metal >= 0) leg.metals.push_back(term.metal);
                else
                    for (int l = 0; l < planes; ++l) leg.metals.push_back(l);
            }
            if (term.mode == Terminal::Mode::Centre && !leg.shape.empty()) {
                // The middle of the shape it is on, when that is in it; else the point.
                const Pt c = leg.shape.centre();
                bool in = false;
                for (int l : leg.metals)
                    metalIndex[std::size_t(l)].query(Box{c.x, c.y, c.x, c.y}, [&](int j) {
                        const int s = planeShape[std::size_t(l)][std::size_t(j)];
                        if (find(s) == find(planeShape[std::size_t(l)][std::size_t(j)]) && inside(p.metal.at(s).outline, c)
                            && owner[std::size_t(find(s))] == n)
                            in = true;
                        return !in;
                    });
                if (in) leg.point = c;
            }
            leg.tail = term.mode != Terminal::Mode::Anywhere;
        }
    }

    // The static boxes.
    for (int s = 0; s < ns; ++s) {
        const Shape& sh = p.metal.at(s);
        if (sh.layer < 0 || sh.layer >= planes) continue;
        const int own = owner[std::size_t(find(s))];
        for (const Box& b : outerOf[std::size_t(s)])
            statics[std::size_t(sh.layer)].push_back(
                {b, isPad[std::size_t(s)] ? toTerminal : toMetal, isPad[std::size_t(s)] ? Kind::Terminal : Kind::Metal, own, s});
        if (own >= 0) {
            for (const Box& b : innerOf[std::size_t(s)]) {
                ownInner[std::size_t(sh.layer)].push_back(b);
                ownInnerOwner[std::size_t(sh.layer)].push_back(own);
                ownInnerPiece[std::size_t(sh.layer)].push_back(find(s));
            }
        }
    }
    for (int o = 0; o < p.obstacles.size(); ++o) {
        const Shape& sh = p.obstacles.at(o);
        if (sh.layer < 0 || sh.layer >= t.obstacles.size()) continue;
        const ObstacleRule& rule = t.obstacles.at(sh.layer);
        std::vector<Box> outer;
        boxesOf(sh.outline, slab, &outer, nullptr);
        for (int l : rule.metals)
            for (const Box& b : outer) statics[std::size_t(l)].push_back({b, p.units(rule.clearance), Kind::Obstacle, -1, o});
    }
    keepOutNets.assign(std::size_t(p.keepOuts.size()), {});
    for (int k = 0; k < p.keepOuts.size(); ++k) {
        const KeepOut& ko = p.keepOuts.at(k);
        std::vector<Box> outer;
        boxesOf(ko.outline, slab, &outer, nullptr);
        if (ko.letThrough != KeepOut::LetThrough::None)
            for (int n = 0; n < p.nets.size(); ++n) {
                bool in = ko.except.contains(p.nets.at(n).name);
                for (const Terminal& term : p.nets.at(n).terminals) in = in || inside(ko.outline, term.at);
                if (in) keepOutNets[std::size_t(k)].push_back(n);
            }
        for (int l = 0; l < planes; ++l) {
            if (!ko.metals.isEmpty() && !ko.metals.contains(l)) continue;
            for (const Box& b : outer) statics[std::size_t(l)].push_back({b, 0, Kind::KeepOut, -1, k});
        }
    }
    staticIndex.assign(std::size_t(planes), BoxIndex());
    ownIndex.assign(std::size_t(planes), BoxIndex());
    maxClearance = 0;
    for (int l = 0; l < planes; ++l) {
        std::vector<Box> boxes;
        boxes.reserve(statics[std::size_t(l)].size());
        for (const StaticBox& b : statics[std::size_t(l)]) {
            boxes.push_back(b.box);
            maxClearance = std::max(maxClearance, b.clearance);
        }
        staticIndex[std::size_t(l)].build(boxes);
        ownIndex[std::size_t(l)].build(ownInner[std::size_t(l)]);
    }
    return true;
}

void Router::Impl::rasterise()
{
    base.clear();
    base.resize(types.size());
    for (std::size_t k = 0; k < types.size(); ++k) base[k].resize(std::size_t(planes));
    std::vector<std::pair<int, int>> jobs;
    for (int l = 0; l < planes; ++l)
        for (int k : typesOnPlane[std::size_t(l)]) {
            base[std::size_t(k)][std::size_t(l)].init(cells, &memory);
            jobs.push_back({k, l});
        }
    Pool pool(p.options.threads);
    pool.run(int(jobs.size()), [&](int i, int) {
        const auto [k, l] = jobs[std::size_t(i)];
        Bits& bits = base[std::size_t(k)][std::size_t(l)];
        const Coord half = types[std::size_t(k)].half;
        for (const StaticBox& b : statics[std::size_t(l)])
            raster(b.box, half, b.clearance, [&](int j, int i0, int i1) { bits.setRange(cellAt(i0, j), cellAt(i1, j)); });
    });
}

// Whether a square of the type at 'centre' keeps clear of everything fixed
// that is not the net's own - testing only what lies outside its own metal.
// In a keep-out it may go into: clear, and costly.
bool Router::Impl::exactClear(int net, int type, int plane, Pt centre, bool* costly) const
{
    const Coord half = types[std::size_t(type)].half;
    const Box sq{centre.x - half, centre.y - half, centre.x + half, centre.y + half};
    std::vector<const StaticBox*> foreign;
    std::vector<Box> own;
    bool landing = type != nets[std::size_t(net)].lineType;
    *costly = false;
    bool onOwn = false;
    staticIndex[std::size_t(plane)].query(sq.grown(maxClearance), [&](int i) {
        const StaticBox& b = statics[std::size_t(plane)][std::size_t(i)];
        if (b.owner == net) return true;
        if (b.kind == Kind::KeepOut) {
            const auto& let = keepOutNets[std::size_t(b.source)];
            if (std::find(let.begin(), let.end(), net) != let.end()) {
                if (b.box.overlaps(sq) && p.keepOuts.at(b.source).letThrough == KeepOut::LetThrough::WayIn) *costly = true;
                return true;
            }
        }
        if (tooNear(sq, b.box, b.clearance)) foreign.push_back(&b);
        return true;
    });
    if (foreign.empty()) return true;
    ownIndex[std::size_t(plane)].query(sq, [&](int i) {
        if (ownInnerOwner[std::size_t(plane)][std::size_t(i)] == net) {
            own.push_back(ownInner[std::size_t(plane)][std::size_t(i)]);
            onOwn = onOwn || ownInner[std::size_t(plane)][std::size_t(i)].contains(centre);
        }
        return true;
    });
    if (own.empty()) return false;
    // A landing on the net's own contact may sit over an obstacle that allows it.
    if (landing && onOwn)
        foreign.erase(std::remove_if(foreign.begin(), foreign.end(),
                                     [&](const StaticBox* b) {
                                         return b->kind == Kind::Obstacle && p.tech.obstacles.at(p.obstacles.at(b->source).layer).landings;
                                     }),
                      foreign.end());
    const Coord least = std::max<Coord>(1, std::min<Coord>(p.units(0.05), std::max<Coord>(1, half / 16)));
    // The quadtree: a square is clear if it lies in one of the own boxes,
    // or nothing foreign is too near it; else its quarters are.
    const std::function<bool(const Box&, int)> clear = [&](const Box& s, int depth) -> bool {
        for (const Box& o : own)
            if (o.contains(s)) return true;
        bool near = false;
        for (const StaticBox* f : foreign)
            if (tooNear(s, f->box, f->clearance)) {
                near = true;
                break;
            }
        if (!near) return true;
        bool touchesOwn = false;
        for (const Box& o : own) touchesOwn = touchesOwn || o.overlaps(s);
        if (!touchesOwn || s.width() <= least || depth > 24) return false;
        const Coord mx = s.x0 + s.width() / 2, my = s.y0 + s.height() / 2;
        return clear({s.x0, s.y0, mx, my}, depth + 1) && clear({mx, s.y0, s.x1, my}, depth + 1) && clear({s.x0, my, mx, s.y1}, depth + 1)
               && clear({mx, my, s.x1, s.y1}, depth + 1);
    };
    return clear(sq, 0);
}

void Router::Impl::buildZone(int net, int type, int plane, Zone* z)
{
    z->type = type;
    z->plane = plane;
    const Coord reach = types[std::size_t(type)].half + maxClearance + pitch;
    std::vector<Box> regions;
    for (std::size_t i = 0; i < statics[std::size_t(plane)].size(); ++i) {
        const StaticBox& b = statics[std::size_t(plane)][i];
        bool mine = b.owner == net;
        if (!mine && b.kind == Kind::KeepOut) {
            const auto& let = keepOutNets[std::size_t(b.source)];
            mine = std::find(let.begin(), let.end(), net) != let.end();
        }
        if (mine) regions.push_back(b.box.grown(reach));
    }
    if (regions.empty()) return;
    const Bits& bits = base[std::size_t(type)][std::size_t(plane)];
    // Pages the regions touch, each cell of them the base blocks tested once.
    std::vector<std::uint32_t> pages;
    const int px = (nx + Page - 1) / Page;
    for (const Box& r : regions) {
        const int i0 = int(std::clamp<Coord>((r.x0 - ox) / pitch, 0, nx - 1)), i1 = int(std::clamp<Coord>((r.x1 - ox) / pitch + 1, 0, nx - 1));
        const int j0 = int(std::clamp<Coord>((r.y0 - oy) / pitch, 0, ny - 1)), j1 = int(std::clamp<Coord>((r.y1 - oy) / pitch + 1, 0, ny - 1));
        for (int pj = j0 / Page; pj <= j1 / Page; ++pj)
            for (int pi = i0 / Page; pi <= i1 / Page; ++pi) pages.push_back(std::uint32_t(pj * px + pi));
    }
    std::sort(pages.begin(), pages.end());
    pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
    for (std::uint32_t pg : pages) {
        const int pi = int(pg % std::uint32_t(px)), pj = int(pg / std::uint32_t(px));
        std::vector<std::uint8_t> values(Page * Page, Unknown);
        bool any = false;
        for (int j = pj * Page; j < std::min(ny, (pj + 1) * Page); ++j)
            for (int i = pi * Page; i < std::min(nx, (pi + 1) * Page); ++i) {
                const std::size_t c = cellAt(i, j);
                if (!bits.test(c)) continue;
                const Pt centre = pointOf(c);
                bool inRegion = false;
                for (const Box& r : regions) inRegion = inRegion || r.contains(centre);
                if (!inRegion) continue;
                bool costly = false;
                const bool ok = exactClear(net, type, plane, centre, &costly);
                values[std::size_t((j - pj * Page) * Page + (i - pi * Page))] = ok ? (costly ? Costly : Free) : Blocked;
                any = any || ok;
            }
        if (any) {
            memory.add(Page * Page);
            z->pages.emplace(pg, std::move(values));
        }
    }
}

void Router::Impl::buildTerminals(int n)
{
    NetState& st = nets[std::size_t(n)];
    const Net& net = p.nets.at(n);
    if (net.fixed) return;
    // Zones: the net's line on each plane, its landings on theirs.
    std::vector<std::pair<int, int>> wanted;
    for (int l = 0; l < planes; ++l) wanted.push_back({st.lineType, l});
    for (int v = 0; v < p.tech.vias.size(); ++v) {
        wanted.push_back({classLandLower[std::size_t(st.cls)][std::size_t(v)], p.tech.vias.at(v).lower});
        wanted.push_back({classLandUpper[std::size_t(st.cls)][std::size_t(v)], p.tech.vias.at(v).lower + 1});
    }
    std::sort(wanted.begin(), wanted.end());
    wanted.erase(std::unique(wanted.begin(), wanted.end()), wanted.end());
    for (const auto& [type, plane] : wanted) {
        Zone z;
        buildZone(n, type, plane, &z);
        if (!z.pages.empty()) st.zones.push_back(std::move(z));
    }

    // Its window: its terminals and guide, grown.
    Box b;
    for (const Leg& leg : st.terminals) b = b.united(Box{leg.point.x, leg.point.y, leg.point.x, leg.point.y});
    for (const Pt& g : net.guide) b = b.united(Box{g.x, g.y, g.x, g.y});
    const Coord grow = std::max<Coord>(pitch * 40, std::max(b.width(), b.height()) / 4);
    const Box w = b.grown(grow);
    st.window = {std::clamp<Coord>((w.x0 - ox) / pitch, 0, nx - 1), std::clamp<Coord>((w.y0 - oy) / pitch, 0, ny - 1),
                 std::clamp<Coord>((w.x1 - ox) / pitch + 1, 0, nx - 1), std::clamp<Coord>((w.y1 - oy) / pitch + 1, 0, ny - 1)};

    // Each terminal's cells: inside its shape (centre, exact) or anywhere on
    // its own metal (anywhere); a terminal on no shape, the cells round its point.
    std::vector<std::uint8_t*> table;
    const int px = (nx + Page - 1) / Page, py = (ny + Page - 1) / Page;
    for (int ti = 0; ti < int(st.terminals.size()); ++ti) {
        Leg& leg = st.terminals[std::size_t(ti)];
        const Terminal& term = net.terminals.at(ti);
        std::vector<quint64> found;
        for (int l : leg.metals) {
            const Zone* zone = nullptr;
            for (const Zone& z : st.zones)
                if (z.type == st.lineType && z.plane == l) zone = &z;
            table.assign(std::size_t(px) * std::size_t(py), nullptr);
            if (zone)
                for (const auto& [pg, v] : zone->pages) table[pg] = const_cast<std::uint8_t*>(v.data());
            const auto consider = [&](int i, int j) {
                if (i < 0 || j < 0 || i >= nx || j >= ny) return;
                const std::size_t c = cellAt(i, j);
                if (staticAt(st, st.lineType, l, c, &table) != Blocked) found.push_back(packState(l, c));
            };
            if (leg.shape.empty()) {
                const Coord fi = Coord(std::floor(double(term.at.x - ox) / double(pitch)));
                const Coord fj = Coord(std::floor(double(term.at.y - oy) / double(pitch)));
                for (int dj = 0; dj <= 1; ++dj)
                    for (int di = 0; di <= 1; ++di) consider(int(fi + di), int(fj + dj));
                continue;
            }
            for (std::size_t k = 0; k < ownInner[std::size_t(l)].size(); ++k) {
                if (ownInnerOwner[std::size_t(l)][k] != n) continue;
                // Its own piece only: the net's other terminals are its own metal too.
                if (std::find(leg.pieces.begin(), leg.pieces.end(), ownInnerPiece[std::size_t(l)][k]) == leg.pieces.end()) continue;
                const Box& ib = ownInner[std::size_t(l)][k];
                if (term.mode != Terminal::Mode::Anywhere && !leg.shape.contains(ib)) continue;
                const Coord i0 = std::max<Coord>(0, Coord(std::ceil(double(ib.x0 - ox) / double(pitch))));
                const Coord i1 = std::min<Coord>(nx - 1, Coord(std::floor(double(ib.x1 - ox) / double(pitch))));
                const Coord j0 = std::max<Coord>(0, Coord(std::ceil(double(ib.y0 - oy) / double(pitch))));
                const Coord j1 = std::min<Coord>(ny - 1, Coord(std::floor(double(ib.y1 - oy) / double(pitch))));
                for (Coord j = j0; j <= j1; ++j)
                    for (Coord i = i0; i <= i1; ++i) consider(int(i), int(j));
            }
        }
        std::sort(found.begin(), found.end());
        found.erase(std::unique(found.begin(), found.end()), found.end());
        for (quint64 s : found) {
            const Pt c = pointOf(cellOf(s));
            // Only a cell whose tail into the terminal's middle can be drawn.
            if (leg.tail && !tailClear(n, planeOf(s), c, leg.point)) continue;
            const float tail = leg.tail ? float(double(std::abs(c.x - leg.point.x) + std::abs(c.y - leg.point.y)) / double(pitch)) : 0.f;
            leg.cells.push_back({s, tail});
        }
        if (term.mode == Terminal::Mode::Anywhere) {
            std::vector<int> shapes;
            for (int l = 0; l < planes; ++l)
                for (std::size_t k = 0; k < ownInner[std::size_t(l)].size(); ++k)
                    if (std::find(leg.pieces.begin(), leg.pieces.end(), ownInnerPiece[std::size_t(l)][k]) != leg.pieces.end()) shapes.push_back(int(k));
            leg.joined = int(shapes.size());
        }
    }

    // A guide's band, over its window.
    if (net.guide.size() >= 2) {
        const Coord band = net.band > 0 ? net.band : 3 * (p.units(p.tech.classes.at(st.cls).width) + types[std::size_t(st.lineType)].spacing);
        const int ww = int(st.window.x1 - st.window.x0 + 1), wh = int(st.window.y1 - st.window.y0 + 1);
        st.guideBand.init(std::size_t(ww) * std::size_t(wh), &memory);
        st.hasGuide = true;
        for (int j = 0; j < wh; ++j)
            for (int i = 0; i < ww; ++i) {
                const Pt c = pointOf(cellAt(int(st.window.x0) + i, int(st.window.y0) + j));
                bool in = st.terminals.front().shape.grown(band).contains(c)
                          || double(c.x - st.terminals.front().point.x) * double(c.x - st.terminals.front().point.x)
                                     + double(c.y - st.terminals.front().point.y) * double(c.y - st.terminals.front().point.y)
                                 <= double(band) * double(band);   // the start always: the line must leave it
                for (int g = 0; g + 1 < net.guide.size() && !in; ++g)
                    in = segmentDistance2(c, c, net.guide.at(g), net.guide.at(g + 1)) <= double(band) * double(band);
                if (in) st.guideBand.set(std::size_t(j) * std::size_t(ww) + std::size_t(i));
            }
    }
}

// Whether a kept route's cells are still free of what is fixed: its line
// at each cell, its landings at each change of metal.
bool Router::Impl::runClear(int n, int plane, Pt a, Pt b) const
{
    if (a == b) return true;
    const NetState& st = nets[std::size_t(n)];
    const Coord half = types[std::size_t(st.lineType)].half;
    const Coord step = std::max<Coord>(1, std::min<Coord>(pitch, half));
    const Coord len = std::abs(b.x - a.x) + std::abs(b.y - a.y);
    for (Coord d = 0;; d = std::min(len, d + step)) {
        const Pt c{a.x + (b.x - a.x) * d / len, a.y + (b.y - a.y) * d / len};
        bool costly;
        if (!exactClear(n, st.lineType, plane, c, &costly)) return false;
        if (d == len) break;
    }
    return true;
}

bool Router::Impl::tailClear(int n, int plane, Pt from, Pt to, Pt* corner, bool horizontalFirst) const
{
    const Pt first = horizontalFirst ? Pt{to.x, from.y} : Pt{from.x, to.y};
    const Pt second = horizontalFirst ? Pt{from.x, to.y} : Pt{to.x, from.y};
    if (runClear(n, plane, from, first) && runClear(n, plane, first, to)) {
        if (corner) *corner = first;
        return true;
    }
    if (first != second && runClear(n, plane, from, second) && runClear(n, plane, second, to)) {
        if (corner) *corner = second;
        return true;
    }
    return false;
}

bool Router::Impl::stillClear(int n) const
{
    const NetState& st = nets[std::size_t(n)];
    const int px = (nx + Page - 1) / Page, py = (ny + Page - 1) / Page;
    std::vector<std::vector<std::uint8_t*>> tables(types.size() * std::size_t(planes));
    for (const Zone& z : st.zones) {
        auto& tab = tables[std::size_t(z.type) * std::size_t(planes) + std::size_t(z.plane)];
        tab.assign(std::size_t(px) * std::size_t(py), nullptr);
        for (const auto& [pg, v] : z.pages) tab[pg] = const_cast<std::uint8_t*>(v.data());
    }
    const auto clear = [&](int type, int plane, std::size_t c) {
        const auto& tab = tables[std::size_t(type) * std::size_t(planes) + std::size_t(plane)];
        return staticAt(st, type, plane, c, tab.empty() ? nullptr : &tab) != Blocked;
    };
    std::vector<quint64> sorted = st.path;
    std::sort(sorted.begin(), sorted.end());
    for (quint64 s : sorted) {
        const int l = planeOf(s);
        const std::size_t c = cellOf(s);
        if (!clear(st.lineType, l, c)) return false;
        // The same cell on the metal above: a change of metal, its landings.
        if (l + 1 < planes && std::binary_search(sorted.begin(), sorted.end(), packState(l + 1, c))
            && (!clear(classLandLower[std::size_t(st.cls)][std::size_t(l)], l, c) || !clear(classLandUpper[std::size_t(st.cls)][std::size_t(l)], l + 1, c)))
            return false;
    }
    return true;
}

std::uint8_t Router::Impl::staticAt(const NetState&, int type, int plane, std::size_t c, const std::vector<std::uint8_t*>* table) const
{
    if (!base[std::size_t(type)][std::size_t(plane)].test(c)) return Free;
    if (!table) return Blocked;
    const int i = int(c % std::size_t(nx)), j = int(c / std::size_t(nx));
    const int px = (nx + Page - 1) / Page;
    const std::uint8_t* pg = (*table)[std::size_t(j / Page) * std::size_t(px) + std::size_t(i / Page)];
    if (!pg) return Blocked;
    const std::uint8_t v = pg[(j % Page) * Page + (i % Page)];
    return v == Unknown ? Blocked : v;
}

void Router::Impl::estimateCrowding()
{
    // RUDY: each two-point leg's length spread over its box, against the
    // free share of the cells, tile by tile.
    const int tile = 32;
    const int tx = (nx + tile - 1) / tile, ty = (ny + tile - 1) / tile;
    if (tx * ty > 4000000) return;
    std::vector<double> demand(std::size_t(tx) * std::size_t(ty), 0), free(std::size_t(tx) * std::size_t(ty), 0);
    const int line = classLine[std::size_t(p.tech.defaultClass)];
    const double track = double(2 * types[std::size_t(line)].half + types[std::size_t(line)].spacing);
    for (int l = 0; l < planes; ++l) {
        const Bits& b = base[std::size_t(line)][std::size_t(l)];
        for (int j = 0; j < ny; j += 2)
            for (int i = 0; i < nx; i += 2)
                if (!b.test(cellAt(i, j))) free[std::size_t(j / tile) * std::size_t(tx) + std::size_t(i / tile)] += 4;
    }
    for (int n = 0; n < p.nets.size(); ++n) {
        const NetState& st = nets[std::size_t(n)];
        if (p.nets.at(n).fixed || st.terminals.size() < 2) continue;
        const Coord trackW = 2 * types[std::size_t(st.lineType)].half + types[std::size_t(st.lineType)].spacing;
        for (std::size_t t = 1; t < st.terminals.size(); ++t) {
            const Pt a = st.terminals.front().point, b = st.terminals[t].point;
            const double w = std::max<double>(double(std::abs(a.x - b.x)), double(trackW)), h = std::max<double>(double(std::abs(a.y - b.y)), double(trackW));
            const double density = (w + h) * double(trackW) / (w * h);   // the share of the box its line covers
            const int i0 = int(std::clamp<Coord>((std::min(a.x, b.x) - ox) / pitch / tile, 0, tx - 1));
            const int i1 = int(std::clamp<Coord>((std::max(a.x, b.x) - ox) / pitch / tile, 0, tx - 1));
            const int j0 = int(std::clamp<Coord>((std::min(a.y, b.y) - oy) / pitch / tile, 0, ty - 1));
            const int j1 = int(std::clamp<Coord>((std::max(a.y, b.y) - oy) / pitch / tile, 0, ty - 1));
            for (int j = j0; j <= j1; ++j)
                for (int i = i0; i <= i1; ++i) demand[std::size_t(j) * std::size_t(tx) + std::size_t(i)] += density * double(tile * tile);
        }
    }
    (void)track;
    // Tiles at a terminal are not reported: lines meet at a terminal by
    // design, and the room right round one is what it is (the black-box
    // test of 7 November had every job warned of, 180 to 670 %, all at the
    // capacitors' edges).
    std::vector<char> atTerminal(std::size_t(tx) * std::size_t(ty), 0);
    for (const NetState& st : nets)
        for (const Leg& leg : st.terminals) {
            const Box b = (leg.shape.empty() ? Box{leg.point.x, leg.point.y, leg.point.x, leg.point.y} : leg.shape).grown(pitch * tile);
            const int i0 = int(std::clamp<Coord>((b.x0 - ox) / pitch / tile, 0, tx - 1)), i1 = int(std::clamp<Coord>((b.x1 - ox) / pitch / tile, 0, tx - 1));
            const int j0 = int(std::clamp<Coord>((b.y0 - oy) / pitch / tile, 0, ty - 1)), j1 = int(std::clamp<Coord>((b.y1 - oy) / pitch / tile, 0, ty - 1));
            for (int j = j0; j <= j1; ++j)
                for (int i = i0; i <= i1; ++i) atTerminal[std::size_t(j) * std::size_t(tx) + std::size_t(i)] = 1;
        }
    // Over 3 x 3 tiles, so that a tile of solid metal says nothing: lines
    // go round it, through its neighbours.
    std::vector<std::pair<double, int>> tiles;
    const double room = double(tile) * tile * planes;
    for (int j = 0; j < ty; ++j)
        for (int i = 0; i < tx; ++i) {
            double d = 0, c = 0, cells = 0;
            for (int dj = -1; dj <= 1; ++dj)
                for (int di = -1; di <= 1; ++di) {
                    const int a = i + di, b = j + dj;
                    if (a < 0 || b < 0 || a >= tx || b >= ty) continue;
                    d += demand[std::size_t(b) * std::size_t(tx) + std::size_t(a)];
                    c += free[std::size_t(b) * std::size_t(tx) + std::size_t(a)];
                    cells += room;
                }
            if (d <= 0 || c < 0.15 * cells || atTerminal[std::size_t(j) * std::size_t(tx) + std::size_t(i)]) continue;
            const double u = d / c;
            // 70 %: on the 200-net chip, outside its terminals, the easy jobs
            // came to 39 and 48 %, the one that stalled to 85 %.
            if (u >= Crowded) tiles.push_back({u, j * tx + i});
        }
    std::sort(tiles.begin(), tiles.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (std::size_t k = 0; k < tiles.size() && k < 5; ++k) {
        const int i = tiles[k].second % tx, j = tiles[k].second / tx;
        Crowding c;
        c.where = {ox + Coord(i * tile) * pitch, oy + Coord(j * tile) * pitch, ox + Coord(std::min(nx - 1, (i + 1) * tile)) * pitch,
                   oy + Coord(std::min(ny - 1, (j + 1) * tile)) * pitch};
        c.use = tiles[k].first;
        crowding.append(c);
    }
}

quint64 Router::Impl::key(int n) const
{
    QCryptographicHash h(QCryptographicHash::Sha1);
    const auto add = [&](const void* data, qsizetype size) { h.addData(QByteArrayView(static_cast<const char*>(data), size)); };
    const auto addInt = [&](qint64 v) { add(&v, sizeof v); };
    const Net& net = p.nets.at(n);
    const QByteArray tech = QJsonDocument(technologyJson(p.tech)).toJson(QJsonDocument::Compact);
    h.addData(tech);
    addInt(pitch);
    addInt(net.netClass);
    for (const Terminal& t : net.terminals) {
        addInt(t.at.x);
        addInt(t.at.y);
        addInt(t.metal);
        addInt(int(t.mode));
    }
    for (const Pt& g : net.guide) {
        addInt(g.x);
        addInt(g.y);
    }
    addInt(net.band);
    // What is fixed in its window.
    const NetState& st = nets[std::size_t(n)];
    const Box w{ox + st.window.x0 * pitch, oy + st.window.y0 * pitch, ox + st.window.x1 * pitch, oy + st.window.y1 * pitch};
    for (int l = 0; l < planes; ++l) {
        std::vector<int> ids;
        staticIndex[std::size_t(l)].query(w, [&](int i) {
            ids.push_back(i);
            return true;
        });
        std::sort(ids.begin(), ids.end(), [&](int a, int b) {
            const Box& x = statics[std::size_t(l)][std::size_t(a)].box;
            const Box& y = statics[std::size_t(l)][std::size_t(b)].box;
            return std::tie(x.x0, x.y0, x.x1, x.y1, a) < std::tie(y.x0, y.y0, y.x1, y.y1, b);
        });
        for (int i : ids) {
            const StaticBox& b = statics[std::size_t(l)][std::size_t(i)];
            addInt(b.box.x0);
            addInt(b.box.y0);
            addInt(b.box.x1);
            addInt(b.box.y1);
            addInt(b.clearance);
            addInt(int(b.kind));
            addInt(b.owner == n ? 1 : 0);
        }
    }
    const QByteArray r = h.result();
    quint64 k = 0;
    std::memcpy(&k, r.constData(), sizeof k);
    return k;
}

// ----------------------------------------------------------------------
// The search

bool Router::Impl::search(int n, const std::vector<std::pair<quint64, float>>& sources, const Leg& target, const Box& window, bool strict,
                          double present, Buffers& b, std::vector<quint64>* path, const Bits* band, int bandTarget, Pt* nearest) const
{
    const NetState& st = nets[std::size_t(n)];
    const Technology& t = p.tech;
    const int wi0 = int(window.x0), wj0 = int(window.y0), wi1 = int(window.x1), wj1 = int(window.y1);
    const int ww = wi1 - wi0 + 1, wh = wj1 - wj0 + 1;
    const std::size_t states = std::size_t(ww) * std::size_t(wh) * std::size_t(planes);
    if (states >= std::numeric_limits<std::uint32_t>::max() - 1) return false;
    b.ensure(states + 1, const_cast<Memory*>(&memory));
    b.reset();
    const std::uint32_t sink = std::uint32_t(states);
    // The band's own window is the net's; this search's may be the whole area.
    const int bwi0 = int(st.window.x0), bwj0 = int(st.window.y0), bww = int(st.window.x1 - st.window.x0 + 1);
    const int bwh = int(st.window.y1 - st.window.y0 + 1);
    const auto inBand = [&](int i, int j) {
        if (!band) return true;
        const int bi = i - bwi0, bj = j - bwj0;
        if (bi < 0 || bj < 0 || bi >= bww || bj >= bwh) return false;
        return band->test(std::size_t(bj) * std::size_t(bww) + std::size_t(bi));
    };
    const auto local = [&](int i, int j, int l) { return std::uint32_t((std::size_t(j - wj0) * std::size_t(ww) + std::size_t(i - wi0)) * std::size_t(planes) + std::size_t(l)); };

    // Zone tables, one a type and plane the search asks about.
    const int px = (nx + Page - 1) / Page, py = (ny + Page - 1) / Page;
    std::vector<std::vector<std::uint8_t*>> tables(types.size() * std::size_t(planes));
    for (const Zone& z : st.zones) {
        auto& tab = tables[std::size_t(z.type) * std::size_t(planes) + std::size_t(z.plane)];
        tab.assign(std::size_t(px) * std::size_t(py), nullptr);
        for (const auto& [pg, v] : z.pages) tab[pg] = const_cast<std::uint8_t*>(v.data());
    }
    const auto tableOf = [&](int type, int plane) -> const std::vector<std::uint8_t*>* {
        const auto& tab = tables[std::size_t(type) * std::size_t(planes) + std::size_t(plane)];
        return tab.empty() ? nullptr : &tab;
    };
    const auto statAt = [&](int type, int plane, std::size_t c) { return staticAt(st, type, plane, c, tableOf(type, plane)); };

    // Targets: a cell of the target leg's and its tail.
    std::unordered_map<std::uint32_t, float> targets;
    Pt aim = target.point;
    Box aimBox{aim.x, aim.y, aim.x, aim.y};
    if (!target.tail && !target.shape.empty()) aimBox = target.shape;
    if (bandTarget >= 0) {
        // The guide's end: any cell of the band near its last point.
        const Pt end = p.nets.at(n).guide.back();
        aim = end;
        aimBox = Box{end.x, end.y, end.x, end.y};
        const Coord r = std::max<Coord>(pitch * 2, p.nets.at(n).band > 0 ? p.nets.at(n).band : pitch * 4);
        for (int l = 0; l < planes; ++l)
            for (Coord j = (end.y - r - oy) / pitch; j <= (end.y + r - oy) / pitch + 1; ++j)
                for (Coord i = (end.x - r - ox) / pitch; i <= (end.x + r - ox) / pitch + 1; ++i) {
                    if (i < wi0 || j < wj0 || i > wi1 || j > wj1) continue;
                    const Pt c = pointOf(cellAt(int(i), int(j)));
                    if (double(c.x - end.x) * double(c.x - end.x) + double(c.y - end.y) * double(c.y - end.y) > double(r) * double(r)) continue;
                    if (statAt(st.lineType, l, cellAt(int(i), int(j))) == Blocked) continue;
                    targets.emplace(local(int(i), int(j), l), 0.f);
                }
    } else {
        for (const auto& [s, tail] : target.cells) {
            const std::size_t c = cellOf(s);
            const int i = int(c % std::size_t(nx)), j = int(c / std::size_t(nx));
            if (i < wi0 || j < wj0 || i > wi1 || j > wj1) continue;
            targets.emplace(local(i, j, planeOf(s)), tail);
        }
    }
    if (targets.empty()) return false;
    const float weight = strict ? 1.0f : float(p.options.searchWeight);
    const auto heuristic = [&](int i, int j) {
        const Coord x = ox + Coord(i) * pitch, y = oy + Coord(j) * pitch;
        const Coord dx = std::max<Coord>({0, aimBox.x0 - x, x - aimBox.x1}), dy = std::max<Coord>({0, aimBox.y0 - y, y - aimBox.y1});
        return weight * float(double(dx + dy) / double(pitch));
    };
    const auto congestion = [&](int type, int plane, int ci, int cj, bool* blocked) {
        std::uint16_t u = use[std::size_t(type)][std::size_t(plane)].get(ci, cj);
        if (strict && u > 0) {
            *blocked = true;
            return 0.f;
        }
        const float h = history[std::size_t(plane)].get(ci, cj);
        return float((1.0 + h) * (1.0 + present * u));
    };
    const auto push = [&](std::uint32_t s, float g, std::uint8_t move, float f) {
        if (g >= b.g[s]) return;
        if (b.g[s] == Infinite) b.touched.push_back(s);
        b.g[s] = g;
        b.from[s] = move;
        b.heap.push(f, s);
    };
    for (const auto& [s, tail] : sources) {
        const std::size_t c = cellOf(s);
        const int i = int(c % std::size_t(nx)), j = int(c / std::size_t(nx)), l = planeOf(s);
        if (i < wi0 || j < wj0 || i > wi1 || j > wj1) continue;
        push(local(i, j, l), tail, Source, tail + Tie * heuristic(i, j));
    }
    float sinkG = Infinite;
    std::uint32_t sinkFrom = 0;
    quint64 count = 0;
    float bestH = Infinite;
    std::uint32_t bestState = 0;
    bool any = false;
    const double bend = t.bendCost, via = t.viaCost, wrong = t.wrongWayCost;
    while (!b.heap.empty()) {
        double f;
        const std::uint32_t s = b.heap.pop(&f);
        if (s == sink) break;
        if (b.closed[s]) continue;
        b.closed[s] = 1;
        ++count;
        const float g = b.g[s];
        const int l = int(s % std::uint32_t(planes));
        const std::uint32_t cell = s / std::uint32_t(planes);
        const int i = wi0 + int(cell % std::uint32_t(ww)), j = wj0 + int(cell / std::uint32_t(ww));
        const float hs = heuristic(i, j);
        if (hs < bestH) {
            bestH = hs;
            bestState = s;
            any = true;
        }
        const auto it = targets.find(s);
        if (it != targets.end() && g + it->second < sinkG) {
            sinkG = g + it->second;
            sinkFrom = s;
            b.heap.push(sinkG, sink);
        }
        if (f >= sinkG) break;
        const std::uint8_t arrived = b.from[s];
        const char dir = t.metals.at(l).direction;
        for (int m = 0; m < 4; ++m) {
            const int di = m == PlusX ? 1 : (m == MinusX ? -1 : 0), dj = m == PlusY ? 1 : (m == MinusY ? -1 : 0);
            const int ni = i + di, nj = j + dj;
            if (ni < wi0 || nj < wj0 || ni > wi1 || nj > wj1) continue;
            if (!inBand(ni, nj)) continue;
            const std::size_t nc = cellAt(ni, nj);
            const std::uint8_t sa = statAt(st.lineType, l, nc);
            if (sa == Blocked) continue;
            bool blocked = false;
            const float cong = congestion(st.lineType, l, ni, nj, &blocked);
            if (blocked) continue;
            double step = (dir == 0 || (dir == 'h') == (m <= MinusX)) ? 1.0 : wrong;
            if (sa == Costly) step *= WayInCost;
            double cost = step * cong;
            if (arrived <= MinusY && (arrived <= MinusX) != (m <= MinusX)) cost += bend;
            const std::uint32_t ns = local(ni, nj, l);
            if (b.closed[ns]) continue;
            const float ng = g + float(cost);
            push(ns, ng, std::uint8_t(m), ng + Tie * heuristic(ni, nj));
        }
        for (int up = 0; up < 2; ++up) {
            const int nl = up ? l + 1 : l - 1;
            if (nl < 0 || nl >= planes) continue;
            const int v = std::min(l, nl);
            const int lowT = classLandLower[std::size_t(st.cls)][std::size_t(v)], upT = classLandUpper[std::size_t(st.cls)][std::size_t(v)];
            const std::size_t c = cellAt(i, j);
            if (!cutBlocked.empty() && !cutBlocked[std::size_t(v)].empty() && cutBlocked[std::size_t(v)].test(c)) continue;
            const std::uint8_t a1 = statAt(lowT, v, c), a2 = statAt(upT, v + 1, c);
            if (a1 == Blocked || a2 == Blocked) continue;
            // Not within a cut's spacing of the net's last via on its way here
            // (squares apart by their spacing: up to viaGap cells, or so many
            // steps along a diagonal walk).
            bool nearVia = false;
            {
                const Via& here = t.vias.at(v);
                const Coord size = p.units(here.outline()), gap = p.units(here.spacing);
                std::uint32_t back = s;
                int bi = i - wi0, bj = j - wj0;
                for (int k = 0; k < 2 * viaGap + 2 && !nearVia; ++k) {
                    const std::uint8_t m = b.from[back];
                    if (m == Up || m == Down) {
                        const double dx = double(std::max<Coord>(0, std::abs(Coord(bi - (i - wi0))) * pitch - size));
                        const double dy = double(std::max<Coord>(0, std::abs(Coord(bj - (j - wj0))) * pitch - size));
                        nearVia = dx * dx + dy * dy < double(gap) * double(gap) || (bi == i - wi0 && bj == j - wj0);
                        break;
                    }
                    if (m >= Source) break;
                    switch (m) {
                    case PlusX: --bi; break;
                    case MinusX: ++bi; break;
                    case PlusY: --bj; break;
                    case MinusY: ++bj; break;
                    default: break;
                    }
                    back = std::uint32_t((std::size_t(bj) * std::size_t(ww) + std::size_t(bi)) * std::size_t(planes) + std::size_t(l));
                }
            }
            if (nearVia) continue;
            bool blocked = false;
            const float c1 = congestion(lowT, v, i, j, &blocked);
            const float c2 = congestion(upT, v + 1, i, j, &blocked);
            if (blocked) continue;
            double cost = via * 0.5 * (c1 + c2);
            if (a1 == Costly || a2 == Costly) cost *= WayInCost;
            const std::uint32_t ns = local(i, j, nl);
            if (b.closed[ns]) continue;
            const float ng = g + float(cost);
            push(ns, ng, std::uint8_t(up ? Up : Down), ng + Tie * hs);
        }
    }
    const_cast<std::atomic<quint64>&>(searched) += count;
    b.account();
    if (sinkG == Infinite) {
        if (nearest && any) {
            const std::uint32_t cell = bestState / std::uint32_t(planes);
            *nearest = pointOf(cellAt(wi0 + int(cell % std::uint32_t(ww)), wj0 + int(cell / std::uint32_t(ww))));
        }
        return false;
    }
    // Back from the target to a source.
    path->clear();
    std::uint32_t s = sinkFrom;
    for (std::size_t guard = 0; guard <= states; ++guard) {
        const int l = int(s % std::uint32_t(planes));
        const std::uint32_t cell = s / std::uint32_t(planes);
        const int i = wi0 + int(cell % std::uint32_t(ww)), j = wj0 + int(cell / std::uint32_t(ww));
        path->push_back(packState(l, cellAt(i, j)));
        const std::uint8_t m = b.from[s];
        if (m == Source || m == NoMove) break;
        int pi = i, pj = j, pl = l;
        switch (m) {
        case PlusX: pi = i - 1; break;
        case MinusX: pi = i + 1; break;
        case PlusY: pj = j - 1; break;
        case MinusY: pj = j + 1; break;
        case Up: pl = l - 1; break;
        case Down: pl = l + 1; break;
        default: break;
        }
        s = local(pi, pj, pl);
    }
    std::reverse(path->begin(), path->end());
    return true;
}

// Routes a net: its start to its first pad, then each further pad from what
// is drawn already. Its path and metal are set; false when no way was found.
bool Router::Impl::route(int n, bool strict, double present, Buffers& b, bool fullArea)
{
    NetState& st = nets[std::size_t(n)];
    const Net& net = p.nets.at(n);
    const Box all{0, 0, nx - 1, ny - 1};

    const Box window = fullArea ? all : windowOf(n);
    std::vector<quint64> legPath, tree;
    std::vector<std::vector<quint64>> legs;
    std::vector<std::pair<quint64, float>> sources = st.terminals.front().cells;
    for (std::size_t t = 1; t < st.terminals.size(); ++t) {
        const Leg& target = st.terminals[t];
        if (t == 1 && st.hasGuide) {
            // Along the guide to its end, then on to the pad.
            std::vector<quint64> first;
            const bool ok = search(n, sources, target, window, strict, present, b, &first, &st.guideBand, 0, nullptr);
            if (!ok) return false;
            std::vector<std::pair<quint64, float>> from{{first.back(), 0.f}};
            std::vector<quint64> second;
            if (!search(n, from, target, window, strict, present, b, &second, nullptr, -1, nullptr)) return false;
            legPath = first;
            legPath.insert(legPath.end(), second.begin() + 1, second.end());
        } else if (!search(n, sources, target, window, strict, present, b, &legPath, nullptr, -1, nullptr)) {
            return false;
        }
        legs.push_back(legPath);
        tree.insert(tree.end(), legPath.begin(), legPath.end());
        sources.clear();
        for (quint64 s : tree) sources.push_back({s, 0.f});
    }
    std::sort(tree.begin(), tree.end());
    tree.erase(std::unique(tree.begin(), tree.end()), tree.end());
    st.path = tree;

    // Drawn: runs merged, vias at changes of metal, tails into the terminals.
    NetResult& r = st.result;
    r.runs.clear();
    r.vias.clear();
    r.length = 0;
    const Coord width = p.units(p.tech.classes.at(st.cls).width);
    const auto addRun = [&](int metal, Pt a, Pt c) {
        if (a == c && !r.runs.isEmpty()) {
            // A point: drawn only when nothing else is (a net whose start is its pad).
        }
        r.runs.append({metal, a, c, width});
        r.length += std::abs(a.x - c.x) + std::abs(a.y - c.y);
    };
    const auto tail = [&](const Leg& leg, quint64 s, bool lastWasHorizontal, bool atStart) {
        if (!leg.tail) return;
        const Pt g = pointOf(cellOf(s));
        const Pt to = leg.point;
        if (g == to) return;
        const int l = planeOf(s);
        // An L: along the way the line came first, unless that is not clear
        // (a terminal's cells are only those with a tail that is).
        Pt corner = lastWasHorizontal ? Pt{to.x, g.y} : Pt{g.x, to.y};
        tailClear(n, l, g, to, &corner, lastWasHorizontal);
        (void)atStart;
        if (g != corner) addRun(l, g, corner);
        if (corner != to) addRun(l, corner, to);
    };
    for (std::size_t k = 0; k < legs.size(); ++k) {
        const std::vector<quint64>& lp = legs[k];
        if (lp.empty()) continue;
        // Runs along the leg.
        std::size_t a = 0;
        bool firstHorizontal = true, lastHorizontal = true;
        bool sawMove = false;
        for (std::size_t q = 1; q <= lp.size(); ++q) {
            const bool end = q == lp.size();
            bool breakHere = end;
            if (!end) {
                const int la = planeOf(lp[q - 1]), lb = planeOf(lp[q]);
                if (la != lb) {
                    breakHere = true;
                } else if (q - a >= 2) {
                    const Pt p0 = pointOf(cellOf(lp[a])), p1 = pointOf(cellOf(lp[q - 1])), p2 = pointOf(cellOf(lp[q]));
                    const bool h1 = p0.y == p1.y, h2 = p1.y == p2.y;
                    breakHere = h1 != h2;
                }
            }
            if (!breakHere) continue;
            const Pt pa = pointOf(cellOf(lp[a])), pb = pointOf(cellOf(lp[q - 1]));
            if (pa != pb) {
                const bool h = pa.y == pb.y;
                if (!sawMove) firstHorizontal = h;
                lastHorizontal = h;
                sawMove = true;
                addRun(planeOf(lp[a]), pa, pb);
            }
            if (!end && planeOf(lp[q - 1]) != planeOf(lp[q])) {
                const int v = std::min(planeOf(lp[q - 1]), planeOf(lp[q]));
                r.vias.append({v, pb});
                a = q;
            } else {
                a = q - 1;
            }
        }
        if (k == 0) tail(st.terminals.front(), lp.front(), firstHorizontal, true);
        tail(st.terminals[k + 1], lp.back(), lastHorizontal, false);
    }
    if (r.runs.isEmpty() && r.vias.isEmpty() && !tree.empty()) {
        // Start and pad in one cell: a square of line there.
        const Pt c = pointOf(cellOf(tree.front()));
        r.runs.append({planeOf(tree.front()), c, c, width});
    }
    // Vias the drawing adds over metal already there: none; changes of metal
    // through vias already there show as a path from one metal's cell to the
    // other's without a via of its own - counted where its legs meet own metal.
    (void)net;
    draw(n);
    return true;
}

void Router::Impl::draw(int n)
{
    NetState& st = nets[std::size_t(n)];
    st.metal.clear();
    for (const Run& run : std::as_const(st.result.runs)) st.metal.push_back({run.metal, run.box()});
    for (const ViaPlaced& v : std::as_const(st.result.vias)) {
        const Via& via = p.tech.vias.at(v.via);
        const Coord w = p.units(p.tech.classes.at(st.cls).width);
        const Coord lo = std::max(p.units(via.landingLower), w), up = std::max(p.units(via.landingUpper), w);
        st.metal.push_back({via.lower, Box{v.at.x - lo / 2, v.at.y - lo / 2, v.at.x - lo / 2 + lo, v.at.y - lo / 2 + lo}});
        st.metal.push_back({via.lower + 1, Box{v.at.x - up / 2, v.at.y - up / 2, v.at.x - up / 2 + up, v.at.y - up / 2 + up}});
    }
    // The cells, in lattice coordinates, for a later run.
    st.result.cells.clear();
    for (quint64 s : st.path) {
        const std::size_t c = cellOf(s);
        st.result.cells.append(packLattice(planeOf(s), ox / pitch + Coord(c % std::size_t(nx)), oy / pitch + Coord(c / std::size_t(nx))));
    }
}

void Router::Impl::stamp(int n, int sign)
{
    stampBoxes(nets[std::size_t(n)].metal, nets[std::size_t(n)].lineType, sign);
}

void Router::Impl::stampBoxes(const std::vector<std::pair<int, Box>>& metal, int lineType, int sign)
{
    visitStamps(metal, lineType, [&](int k, int plane, int j, int i0, int i1) {
        Paged<std::uint16_t>& u = use[std::size_t(k)][std::size_t(plane)];
        for (int i = i0; i <= i1; ++i) {
            std::uint16_t& v = u.ref(i, j);
            if (sign > 0) v = std::uint16_t(std::min<int>(65535, v + 1));
            else if (v > 0) --v;
        }
    });
}

// Whether a routed net's cells are too near another net's lines (its own
// taken out for the test).
bool Router::Impl::conflicted(int n)
{
    NetState& st = nets[std::size_t(n)];
    if (!st.routed) return false;
    stamp(n, -1);
    bool bad = false;
    std::vector<std::size_t> marks;
    for (std::size_t k = 0; k < st.path.size() && !bad; ++k) {
        const quint64 s = st.path[k];
        const int l = planeOf(s);
        const std::size_t c = cellOf(s);
        if (use[std::size_t(st.lineType)][std::size_t(l)].get(c) > 0) bad = true;
        if (k + 1 < st.path.size() && cellOf(st.path[k + 1]) == c && planeOf(st.path[k + 1]) != l) {
            const int v = std::min(l, planeOf(st.path[k + 1]));
            if (use[std::size_t(classLandLower[std::size_t(st.cls)][std::size_t(v)])][std::size_t(v)].get(c) > 0
                || use[std::size_t(classLandUpper[std::size_t(st.cls)][std::size_t(v)])][std::size_t(v + 1)].get(c) > 0)
                bad = true;
        }
    }
    stamp(n, +1);
    return bad;
}

// Why a net found no way, in words, and where.
QString Router::Impl::diagnose(int n, Pt* where)
{
    NetState& st = nets[std::size_t(n)];
    const NetClass& k = p.tech.classes.at(st.cls);
    for (std::size_t t = 0; t < st.terminals.size(); ++t)
        if (st.terminals[t].cells.empty()) {
            *where = st.terminals[t].point;
            return QStringLiteral("its %1 at (%2, %3) cannot be left: no %4 µm line fits there clear of what is around it")
                .arg(t == 0 ? QStringLiteral("start") : QStringLiteral("pad %1").arg(t), micro(p.um(where->x)), micro(p.um(where->y)),
                     micro(k.width));
        }
    Buffers b;
    std::vector<quint64> path;
    Pt nearest = st.terminals.front().point;
    std::vector<std::pair<quint64, float>> sources = st.terminals.front().cells;
    std::size_t failedLeg = 1;
    for (std::size_t t = 1; t < st.terminals.size(); ++t) {
        failedLeg = t;
        if (!search(n, sources, st.terminals[t], Box{0, 0, nx - 1, ny - 1}, false, 0, b, &path, nullptr, -1, &nearest)) break;
        for (quint64 s : path) sources.push_back({s, 0.f});
        failedLeg = 0;
    }
    if (failedLeg == 0) {
        *where = st.terminals.front().point;
        return QStringLiteral("no way within its guide's band: the guide may cross something it cannot");
    }
    *where = nearest;
    // What is in the way there: the nearest fixed thing to that point, on its way to the pad.
    const Pt to = st.terminals[failedLeg].point;
    QString what;
    double best = std::numeric_limits<double>::max();
    for (int l = 0; l < planes; ++l) {
        const Coord reach = types[std::size_t(st.lineType)].half + maxClearance + 2 * pitch;
        staticIndex[std::size_t(l)].query(Box{nearest.x, nearest.y, nearest.x, nearest.y}.grown(reach), [&](int i) {
            const StaticBox& sb = statics[std::size_t(l)][std::size_t(i)];
            if (sb.owner == n) return true;
            const double d = std::sqrt(boxDistance2(sb.box, Box{nearest.x, nearest.y, nearest.x, nearest.y}));
            // Prefer what lies towards the pad.
            const Pt c = sb.box.centre();
            const double towards = double(c.x - nearest.x) * double(to.x - nearest.x) + double(c.y - nearest.y) * double(to.y - nearest.y);
            const double score = d - (towards > 0 ? 0.5 * double(pitch) : 0);
            if (score >= best) return true;
            best = score;
            const QString box = QStringLiteral("(%1, %2 to %3, %4)")
                                    .arg(micro(p.um(sb.box.x0)), micro(p.um(sb.box.y0)), micro(p.um(sb.box.x1)), micro(p.um(sb.box.y1)));
            switch (sb.kind) {
            case Kind::KeepOut: what = QStringLiteral("the keep-out %1").arg(p.keepOuts.at(sb.source).name); break;
            case Kind::Obstacle: what = QStringLiteral("the obstacle %1 %2").arg(p.tech.obstacles.at(p.obstacles.at(sb.source).layer).name, box); break;
            case Kind::Terminal:
                what = QStringLiteral("net %1's terminal on %2 %3").arg(sb.owner >= 0 ? p.nets.at(sb.owner).name : QStringLiteral("?"), p.tech.metals.at(l).name, box);
                break;
            default:
                what = sb.owner >= 0 ? QStringLiteral("metal of net %1 on %2 %3").arg(p.nets.at(sb.owner).name, p.tech.metals.at(l).name, box)
                                     : QStringLiteral("metal on %1 %2").arg(p.tech.metals.at(l).name, box);
            }
            return true;
        });
    }
    QString text = QStringLiteral("no way, whatever the other nets do: from its %1 it comes no nearer than (%2, %3) to its %4 at (%5, %6)")
                       .arg(failedLeg == 1 ? QStringLiteral("start") : QStringLiteral("other pads"), micro(p.um(nearest.x)), micro(p.um(nearest.y)),
                            failedLeg == 1 && st.terminals.size() == 2 ? QStringLiteral("pad") : QStringLiteral("pad %1").arg(failedLeg),
                            micro(p.um(to.x)), micro(p.um(to.y)));
    if (!what.isEmpty()) text += QStringLiteral(", stopped there by ") + what;
    return text;
}

Result Router::Impl::runAll(const QStringList& which, const QVector<Kept>& kept, const Progress& progress)
{
    QElapsedTimer total;
    total.start();
    Result result;
    memory.resetPeak();
    searched = 0;
    for (auto& perType : use)
        for (auto& m : perType) m.clear();
    for (auto& h : history) h.clear();
    result.terminals = readings;
    result.crowding = crowding;
    for (const Crowding& c : std::as_const(crowding))
        if (c.use >= Crowded)
            result.notes << QStringLiteral("Crowded: about %1 % of the tracks between (%2, %3) and (%4, %5) µm are wanted")
                                .arg(qRound(c.use * 100))
                                .arg(micro(p.um(c.where.x0)), micro(p.um(c.where.y0)), micro(p.um(c.where.x1)), micro(p.um(c.where.y1)));

    const int count = int(nets.size());
    std::vector<int> order;
    for (int n = 0; n < count; ++n) {
        NetState& st = nets[std::size_t(n)];
        st.routed = st.failed = st.kept = st.contested = false;
        st.grow = 0;
        st.path.clear();
        st.metal.clear();
        st.result = NetResult();
        st.result.key = st.key;
        if (p.nets.at(n).fixed) {
            st.result.status = NetResult::Status::Fixed;
            continue;
        }
        // Named: routed again. Kept and unchanged: kept. Neither, when only
        // some were asked for: left as it is.
        const bool asked = which.contains(p.nets.at(n).name);
        const Kept* k = nullptr;
        for (const Kept& kk : kept)
            if (kk.net == p.nets.at(n).name) k = &kk;
        if (k && k->route.key == st.key && !asked && (k->route.status == NetResult::Status::Routed || k->route.status == NetResult::Status::Kept)) {
            // Its cells again, if they are still on the lattice and clear.
            bool ok = true;
            st.path.clear();
            for (quint64 lc : k->route.cells) {
                const int l = int(lc >> 60);
                const Coord gx = Coord((lc >> 30) & ((quint64(1) << 30) - 1)) - (Coord(1) << 29);
                const Coord gy = Coord(lc & ((quint64(1) << 30) - 1)) - (Coord(1) << 29);
                const Coord i = gx - ox / pitch, j = gy - oy / pitch;
                if (l >= planes || i < 0 || j < 0 || i >= nx || j >= ny) {
                    ok = false;
                    break;
                }
                st.path.push_back(packState(l, cellAt(int(i), int(j))));
            }
            if (ok && !stillClear(n)) ok = false;
            if (ok) {
                st.result = k->route;
                st.result.status = NetResult::Status::Kept;
                draw(n);
                st.routed = st.kept = true;
                continue;
            }
            st.path.clear();
        }
        if (!which.isEmpty() && !asked && !k) {
            st.result.status = NetResult::Status::NotRun;
            continue;
        }
        order.push_back(n);
    }
    for (int n = 0; n < count; ++n)
        if (nets[std::size_t(n)].kept) stamp(n, +1);
    // Kept routes too near each other are routed again.
    for (int n = 0; n < count; ++n)
        if (nets[std::size_t(n)].kept && conflicted(n)) {
            stamp(n, -1);
            nets[std::size_t(n)].kept = nets[std::size_t(n)].routed = false;
            order.push_back(n);
        }
    int keptCount = 0;
    for (const NetState& st : nets) keptCount += st.kept;
    if (keptCount > 0) result.notes << QStringLiteral("%1 net(s) kept from the last run, %2 routed again").arg(keptCount).arg(order.size());

    // Short nets first: they have the fewest ways round.
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        const Box& wa = nets[std::size_t(a)].window;
        const Box& wb = nets[std::size_t(b)].window;
        return (wa.width() + wa.height()) < (wb.width() + wb.height());
    });

    Pool pool(p.options.threads);
    std::vector<std::unique_ptr<Buffers>> buffers;
    for (int t = 0; t < pool.size(); ++t) buffers.push_back(std::make_unique<Buffers>());
    Coord reach = 0;
    for (const Type& k : types) reach = std::max(reach, 2 * k.half + k.spacing);
    reach = reach / pitch + 2;

    // Nets that cannot be routed at all are found once, alone.
    const auto snapshot = [&] {
        std::vector<NetResult> s;
        for (const NetState& st : nets) s.push_back(st.result);
        return s;
    };
    struct Best {
        int score = std::numeric_limits<int>::max();
        double length = 0;
        int pass = 0;
        std::vector<NetResult> routes;
        std::vector<std::vector<quint64>> paths;
        std::vector<char> routed, failed, contested;
    } best;
    double present = p.options.presentStart;
    bool lean = false;   // one search at a time, memory being near its cap
    const int passes = p.options.negotiate ? p.options.passes : 1;
    int sinceBest = 0;
    bool stoppedEarly = false;
    QString stopped;
    const auto cancelled = [&] { return progress.cancel && progress.cancel->load(); };

    for (int pass = 1; pass <= passes; ++pass) {
        QElapsedTimer timer;
        timer.start();
        const bool strict = !p.options.negotiate;
        // Batches of nets whose windows (and what their lines would stamp) do
        // not meet: searched at once, then drawn in order.
        std::vector<char> taken(order.size(), 0);
        std::size_t at = 0;
        while (at < order.size()) {
            if (cancelled()) break;
            std::vector<int> batch;
            std::vector<Box> boxes;
            for (std::size_t q = at; q < order.size() && q < at + Ahead && int(batch.size()) < (lean ? 1 : Batch); ++q) {
                if (taken[q]) continue;
                const int n = order[q];
                if (nets[std::size_t(n)].failed) {
                    taken[q] = 1;
                    continue;
                }
                const Box w = windowOf(n).grown(reach);
                bool meets = false;
                for (const Box& o : boxes) meets = meets || o.meets(w);
                // A net met is left for a later batch; one the order put before
                // it is never passed by a net it meets.
                boxes.push_back(w);
                if (meets) continue;
                batch.push_back(n);
                taken[q] = 1;
            }
            while (at < order.size() && taken[at]) ++at;
            if (batch.empty()) continue;
            for (int n : batch)
                if (nets[std::size_t(n)].routed) {
                    stamp(n, -1);
                    nets[std::size_t(n)].routed = false;
                }
            std::vector<char> found(batch.size(), 0);
            pool.run(int(batch.size()), [&](int k, int worker) {
                found[std::size_t(k)] = route(batch[std::size_t(k)], strict, present, *buffers[std::size_t(worker)], false);
            });
            for (std::size_t k = 0; k < batch.size(); ++k) {
                const int n = batch[k];
                if (!found[k]) found[k] = route(n, strict, present, *buffers[0], true);
                NetState& st = nets[std::size_t(n)];
                if (found[k]) {
                    st.routed = true;
                    stamp(n, +1);
                } else if (!strict || pass == 1) {
                    // Negotiation never blocks on others: no way at all. (Ordered:
                    // no way around those before it.)
                    st.failed = !strict;
                    st.result.status = NetResult::Status::Failed;
                    st.path.clear();
                    st.metal.clear();
                    st.result.runs.clear();
                    st.result.vias.clear();
                    st.result.length = 0;
                    if (!strict) st.result.reason = diagnose(n, &st.result.where);
                    else {
                        st.result.reason = QStringLiteral("no way around the nets routed before it (ordered routing)");
                        Pt w;
                        diagnose(n, &w);
                        st.result.where = w;
                    }
                    st.result.hasWhere = true;
                }
            }
            // Near the cap: one search at a time, the other threads' arrays
            // given back. Past it even so: stop, with the best pass.
            const qint64 cap = p.options.maxMemory * 1024 * 1024;
            if (!lean && memory.now() > cap / 20 * 17) {
                lean = true;
                for (std::size_t w = 1; w < buffers.size(); ++w) buffers[w] = std::make_unique<Buffers>();
                result.notes << QStringLiteral("Memory came near max_memory_mb (%1 MB) in pass %2: one search at a time from then on")
                                    .arg(p.options.maxMemory)
                                    .arg(pass);
            }
            if (lean && memory.now() > cap) {
                stopped = QStringLiteral("memory");
                break;
            }
        }
        // How it stands.
        PassRow row;
        row.pass = pass;
        row.partial = cancelled() || !stopped.isEmpty();   // stopped part way through
        double length = 0;
        for (int n : order) {
            NetState& st = nets[std::size_t(n)];
            st.contested = st.routed && conflicted(n);
            row.routed += st.routed;
            row.failed += st.failed || (!st.routed && !st.failed);
            row.contested += st.contested;
            if (st.routed) length += p.um(st.result.length);
        }
        for (int n = 0; n < count; ++n)
            if (nets[std::size_t(n)].kept) length += p.um(nets[std::size_t(n)].result.length);
        row.length = length;
        row.seconds = timer.elapsed() / 1000.0;
        result.passes.append(row);
        if (progress.pass) progress.pass(row);
        const int score = row.failed + row.contested;
        if (score < best.score || (score == best.score && length < best.length - 1e-9)) {
            best.score = score;
            best.length = length;
            best.pass = pass;
            best.routes = snapshot();
            best.paths.clear();
            best.routed.clear();
            best.failed.clear();
            best.contested.clear();
            for (const NetState& st : nets) {
                best.paths.push_back(st.path);
                best.routed.push_back(st.routed);
                best.failed.push_back(st.failed);
                best.contested.push_back(st.contested);
            }
            sinceBest = 0;
        } else {
            ++sinceBest;
        }
        if (!stopped.isEmpty()) break;
        if (cancelled()) {
            stopped = QStringLiteral("cancelled");
            break;
        }
        if (row.contested == 0) {
            stopped = strict ? QStringLiteral("ordered") : QStringLiteral("converged");
            break;
        }
        if (p.options.stallPasses > 0 && sinceBest >= p.options.stallPasses) {
            stopped = QStringLiteral("stalled");
            result.notes << QStringLiteral("Stalled: the contested count has not improved on %1 (pass %2) in %3 passes")
                                .arg(best.score)
                                .arg(best.pass)
                                .arg(sinceBest);
            stoppedEarly = true;
            break;
        }
        if (pass == passes) {
            stopped = strict ? QStringLiteral("ordered") : QStringLiteral("passes");
            break;
        }
        // History where nets met, and more for sharing next pass.
        for (int n : order) {
            NetState& st = nets[std::size_t(n)];
            if (!st.contested) continue;
            ++st.grow;
            stamp(n, -1);
            for (quint64 s : st.path) {
                const int l = planeOf(s);
                const std::size_t c = cellOf(s);
                if (use[std::size_t(st.lineType)][std::size_t(l)].get(c) > 0) {
                    const int i = int(c % std::size_t(nx)), j = int(c / std::size_t(nx));
                    history[std::size_t(l)].ref(i, j) += float(p.options.historyGain);
                }
            }
            stamp(n, +1);
        }
        present = std::min(present * p.options.presentGrowth, p.options.presentMost);
    }
    (void)stoppedEarly;

    // The best pass back, then nets still too near another routed again
    // around everything else, one by one; those that cannot be, reported.
    if (best.pass > 0 && best.pass != int(result.passes.size())) {
        for (auto& perType : use)
            for (auto& m : perType) m.clear();
        for (int n = 0; n < count; ++n) {
            NetState& st = nets[std::size_t(n)];
            st.result = best.routes[std::size_t(n)];
            st.path = best.paths[std::size_t(n)];
            st.routed = best.routed[std::size_t(n)];
            st.failed = best.failed[std::size_t(n)];
            st.contested = best.contested[std::size_t(n)];
            if (st.routed) {
                draw(n);
                stamp(n, +1);
            }
        }
    }
    std::vector<int> contested;
    for (int n : order)
        if (nets[std::size_t(n)].routed && nets[std::size_t(n)].contested) contested.push_back(n);
    // (Stopped too: what is kept must be clear of the others.)
    if (!contested.empty()) {
        QElapsedTimer timer;
        timer.start();
        for (int n : contested) {
            stamp(n, -1);
            nets[std::size_t(n)].routed = false;
        }
        for (int n : contested) {
            NetState& st = nets[std::size_t(n)];
            if (route(n, true, 0, *buffers[0], true)) {
                st.routed = true;
                st.contested = false;
                stamp(n, +1);
            } else {
                st.contested = false;
                st.failed = false;
                st.path.clear();
                st.metal.clear();
                st.result.runs.clear();
                st.result.vias.clear();
                st.result.length = 0;
                st.result.status = NetResult::Status::Failed;
                Pt where = st.terminals.front().point;
                // Where it was stuck: its last way through the crowd.
                const auto& bp = best.paths[std::size_t(n)];
                if (!bp.empty()) where = pointOf(cellOf(bp[bp.size() / 2]));
                st.result.where = where;
                st.result.hasWhere = true;
                st.result.reason = QStringLiteral("too crowded: no way that keeps its spacing to the other nets' lines; its best way, which another "
                                                  "line came too near, ran by (%1, %2)")
                                       .arg(micro(p.um(where.x)), micro(p.um(where.y)));
            }
        }
        PassRow row;
        row.pass = int(result.passes.size()) + 1;
        row.cleanup = true;
        for (int n : order) {
            const NetState& st = nets[std::size_t(n)];
            row.routed += st.routed;
            row.failed += !st.routed;
            if (st.routed) row.length += p.um(st.result.length);
        }
        for (int n = 0; n < count; ++n)
            if (nets[std::size_t(n)].kept) row.length += p.um(nets[std::size_t(n)].result.length);
        row.seconds = timer.elapsed() / 1000.0;
        result.passes.append(row);
        if (progress.pass) progress.pass(row);
    }
    for (int n = 0; n < count; ++n) {
        NetState& st = nets[std::size_t(n)];
        if (st.kept) st.result.status = NetResult::Status::Kept;
        else if (st.routed) st.result.status = NetResult::Status::Routed;
        else if (std::find(order.begin(), order.end(), n) != order.end() && st.result.status != NetResult::Status::Failed) {
            st.result.status = NetResult::Status::Failed;
            if (st.result.reason.isEmpty()) st.result.reason = QStringLiteral("the run was stopped (%1) before it was routed").arg(stopped);
        }
        st.result.key = st.key;
        result.nets.append(st.result);
    }
    result.bestPass = best.pass;
    result.stopped = stopped.isEmpty() ? QStringLiteral("nothing") : stopped;
    if (stopped == QLatin1String("memory")) {
        bool all = true;
        for (int n : order) all = all && nets[std::size_t(n)].routed;
        if (all) {
            // Every net routed and clear of the others after all.
            result.stopped = QStringLiteral("converged");
            result.notes << QStringLiteral("The run stopped at max_memory_mb, and its clean-up routed every net");
        }
    }
    if (order.empty()) result.stopped = QStringLiteral("nothing");
    result.statesSearched = searched.load();
    result.peakBytes = memory.peak();
    result.seconds = total.elapsed() / 1000.0;
    // Kept routes that did not hold: a full negotiation, as the proposal asks.
    return result;
}

// ----------------------------------------------------------------------

Router::Router(Problem problem) : d(std::make_unique<Impl>())
{
    d->p = std::move(problem);
}

Router::~Router() = default;

bool Router::prepare()
{
    return d->prepare();
}

QString Router::error() const
{
    return d->error;
}

qint64 Router::estimatedBytes() const
{
    return d->estimate;
}

const QVector<TerminalReading>& Router::terminals() const
{
    return d->readings;
}

const QVector<Crowding>& Router::crowding() const
{
    return d->crowding;
}

void Router::releaseMaps()
{
    d->use.clear();
    d->history.clear();
    d->base.clear();
    d->cutBlocked.clear();
    for (auto& st : d->nets) {
        st.zones.clear();
        st.guideBand = Bits();
        st.path.clear();
        st.path.shrink_to_fit();
        st.metal.clear();
        st.metal.shrink_to_fit();
    }
    d->released = true;
}

const Problem& Router::problem() const
{
    return d->p;
}

quint64 Router::keyOf(int net) const
{
    return net >= 0 && net < int(d->nets.size()) ? d->nets[std::size_t(net)].key : 0;
}

Result Router::run(const QStringList& which, const QVector<Kept>& kept, const Progress& progress)
{
    if ((d->nets.empty() || d->released) && !(d->released = false, d->prepare())) {
        Result r;
        r.stopped = QStringLiteral("nothing");
        r.notes << d->error;
        return r;
    }
    Result r = d->runAll(which, kept, progress);
    // Slots under the notch rule filled, where a fill keeps clear.
    fillNotches(d->p, &r);
    if (!kept.isEmpty() && (r.failed() > 0 || r.stopped == QLatin1String("stalled") || r.stopped == QLatin1String("passes"))) {
        bool anyKept = false;
        for (const NetResult& n : std::as_const(r.nets)) anyKept = anyKept || n.status == NetResult::Status::Kept;
        if (anyKept) {
            // What was kept may be in the way: everything negotiated afresh.
            QStringList all;
            Result full = d->runAll(all, {}, progress);
            full.notes.prepend(QStringLiteral("Routing only what changed failed (%1 net(s)); everything was routed again").arg(r.failed()));
            fillNotches(d->p, &full);
            if (full.failed() <= r.failed()) return full;
        }
    }
    return r;
}

} // namespace qucs_s::router
