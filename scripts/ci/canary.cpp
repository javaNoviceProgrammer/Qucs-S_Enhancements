// One defect a case, chosen by name, each the kind the sanitized job is there
// for: does a build of given flags catch it? n is 2 at run time, so nothing is
// known at compile time. Run by canary.sh, which says caught or missed.
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

static int n;
static int global[4] = {1, 2, 3, 4};

struct Plain { int x; int y; int get() const { return x; } };
enum class Kind : unsigned char { A, B };

// A half-gone object, as a Qt signal reaches it from its base's destructor
// (cd16c76, bb2f2be, 7ce77fd, dd933b7), and an event cast before its type is
// looked at (ecffaaa).
struct Base { virtual ~Base(); void (*going)(Base*) = nullptr; };
Base::~Base() { if (going) going(this); }
struct Derived : Base { int x = 1; virtual int value() { return x; } int plain() { return x; } };
struct Other : Base { int y = 2; };

__attribute__((noinline)) static int* addressOfLocal() { int x = n; int* p = &x; asm("" : "+r"(p)); return p; }
__attribute__((noinline)) static int missingReturn(int a) { if (a == 0) return 1; }

using Case = std::function<int()>;

int main(int argc, char** argv)
{
    n = argc; // 2: the program and the case
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::map<std::string, Case> cases = {
        // Heap
        {"heap_read", [] { int* p = new int[n]; int v = p[n]; printf("%d\n", v); delete[] p; return 0; }},
        {"heap_write", [] { int* p = new int[n]; p[n] = 1; printf("%d\n", p[0]); delete[] p; return 0; }},
        {"heap_read_unused", [] { int* p = new int[n]; int v = p[n]; (void)v; delete[] p; return 0; }},
        {"use_after_free", [] { int* p = new int[n]; p[0] = 1; delete[] p; printf("%d\n", p[0]); return 0; }},
        {"use_after_free_unused", [] { int* p = new int[n]; delete[] p; int v = p[0]; (void)v; return 0; }},
        {"double_free", [] { int* p = (int*)malloc(n); free(p); free(p); return 0; }},
        {"new_free_mismatch", [] { int* p = new int[n]; free(p); return 0; }},
        {"vector_past_end", [] { std::vector<double> v(n); printf("%g\n", v[n]); return 0; }},
        {"iterator_invalidated", [] { std::vector<int> v(n, 1); auto it = v.begin(); v.resize(1000); printf("%d\n", *it); return 0; }},
        {"object_deleted_then_called", [] { Derived* d = new Derived; delete d; printf("%d\n", d->value()); return 0; }},
        {"memcpy_overlap", [] { char b[16] = "abcdefghijklmno"; memcpy(b, b + n, 8); printf("%s\n", b); return 0; }},
        // Through the C library: ASan's interceptors, unless _FORTIFY_SOURCE sends the call past them
        {"memcpy_heap_read", [] { char* h = new char[n]; h[0] = 1; char d[16]; volatile int len = n * 4; memcpy(d, h, len); printf("%d\n", d[0]); delete[] h; return 0; }},
        {"memcpy_heap_write", [] { char* h = new char[n]; char s[16] = {1}; volatile int len = n * 4; memcpy(h, s, len); printf("%d\n", h[0]); delete[] h; return 0; }},
        {"memset_heap", [] { char* h = new char[n]; volatile int len = n * 4; memset(h, 1, len); printf("%d\n", h[0]); delete[] h; return 0; }},
        {"memset_stack", [] { char a[4]; char* q = a; volatile int len = n * 4; memset(q, 1, len); printf("%d\n", q[0]); return 0; }},
        {"strlen_heap", [] { char* h = new char[n]; memset(h, 'x', n); printf("%zu\n", strlen(h)); delete[] h; return 0; }},
        {"strcpy_heap", [] { char* h = new char[n]; std::string s(n * 8, 'x'); strcpy(h, s.c_str()); printf("%d\n", h[0]); delete[] h; return 0; }},
        {"snprintf_heap", [] { char* h = new char[n]; volatile int len = n * 8; snprintf(h, len, "%s", "abcdefghijkl"); printf("%d\n", h[0]); delete[] h; return 0; }},
        // Stack and globals
        {"stack_read", [] { volatile int i = n * 2; int a[4] = {n, n, n, n}; int* q = a; int v = q[i]; printf("%d\n", v); return 0; }},
        {"stack_write", [] { volatile int i = n * 2; int a[4] = {n, n, n, n}; int* q = a; q[i] = 1; printf("%d\n", q[0]); return 0; }},
        {"stack_read_unused", [] { volatile int i = n * 2; int a[4] = {n, n, n, n}; int* q = a; int v = q[i]; (void)v; return 0; }},
        {"stack_buffer_strcpy", [] { char a[4]; std::string s(n * 8, 'x'); strcpy(a, s.c_str()); printf("%s\n", a); return 0; }},
        {"use_after_scope", [] { int* p; { int x = n; p = &x; asm("" : "+r"(p)); } printf("%d\n", *p); return 0; }},
        {"use_after_return", [] { int* p = addressOfLocal(); printf("%d\n", *p); return 0; }},
        {"global_read", [] { volatile int i = n * 2; int* q = global; printf("%d\n", q[i]); return 0; }},
        // Undefined behaviour: arithmetic
        {"signed_add", [] { int v = INT_MAX - 1 + n; printf("%d\n", v); return 0; }},
        {"signed_add_unused", [] { int v = INT_MAX - 1 + n; (void)v; return 0; }},
        {"signed_mul", [] { int v = (INT_MAX / 2 + 1) * n; printf("%d\n", v); return 0; }},
        {"signed_mul_64", [] { long long v = (LLONG_MAX / 2 + 1) * n; printf("%lld\n", v); return 0; }},
        {"signed_loop", [] { int s = INT_MAX - 10; for (int i = 0; i < 10 * n; ++i) s += n; printf("%d\n", s); return 0; }},
        {"negate_min", [] { int v = INT_MIN + n - 2; v = -v; printf("%d\n", v); return 0; }},
        {"shift_too_far", [] { int v = 1 << (n + 40); printf("%d\n", v); return 0; }},
        {"divide_by_zero", [] { int v = 10 / (n - 2); printf("%d\n", v); return 0; }},
        {"pointer_overflow", [] { char* p = (char*)&n; char* q = p + (LLONG_MAX / 2 + n * 1000000LL) * 3; printf("%p\n", (void*)q); return 0; }},
        // Undefined behaviour: pointers and objects
        {"null_load", [] { double d = 1; const double* p = n == 2 ? nullptr : &d; printf("%g\n", *p); return 0; }},
        {"null_member", [] { Plain s{1, 2}; Plain* p = n == 2 ? nullptr : &s; printf("%d\n", p->y); return 0; }},
        {"null_call", [] { Plain s{1, 2}; Plain* p = n == 2 ? nullptr : &s; printf("%d\n", p->get()); return 0; }},
        {"misaligned", [] { alignas(8) char b[16] = {}; int* p = (int*)(b + n - 1); printf("%d\n", *p); return 0; }},
        {"array_index", [] { int a[4] = {}; volatile int i = n * 2; printf("%d\n", a[i]); return 0; }},
        {"object_size", [] { Plain* p = (Plain*)malloc(n - 1); printf("%d\n", p->x); free(p); return 0; }},
        {"half_gone_object", [] { Derived* d = new Derived; d->going = [](Base* b) { printf("%d\n", static_cast<Derived*>(b)->plain()); }; delete d; return 0; }},
        {"wrong_event_cast", [] { Other o; Base* b = &o; printf("%d\n", static_cast<Derived*>(b)->plain()); return 0; }},
        {"wrong_virtual_call", [] { Other o; Base* b = &o; printf("%d\n", static_cast<Derived*>(b)->value()); return 0; }},
        {"missing_return", [] { printf("%d\n", missingReturn(n)); return 0; }},
        {"unreachable", [] { if (n == 2) __builtin_unreachable(); printf("%d\n", n); return 0; }},
        {"invalid_bool", [] { char c = (char)n; bool b; memcpy(&b, &c, 1); printf("%d\n", b ? 1 : 0); return 0; }},
        {"invalid_enum", [] { unsigned char c = (unsigned char)(n + 40); Kind k; memcpy(&k, &c, 1); printf("%d\n", k == Kind::B ? 1 : 0); return 0; }},
        {"memcpy_null", [] { char b[4]; const char* s = n == 2 ? nullptr : "abc"; memcpy(b, s, n - 2); printf("%d\n", (int)sizeof(b)); return 0; }},
        {"ctz_of_zero", [] { printf("%d\n", __builtin_ctz(n - 2)); return 0; }},
    };
    if (argc == 2 && std::string(argv[1]) == "--list") {
        n = 0;
        for (const auto& c : cases) printf("%s\n", c.first.c_str());
        return 0;
    }
    auto it = argc == 2 ? cases.find(argv[1]) : cases.end();
    if (it == cases.end()) { fprintf(stderr, "no such case\n"); return 125; }
    return it->second();
}
