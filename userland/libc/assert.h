/* No include guard on purpose: ISO C says assert.h may be re-included
 * after toggling NDEBUG. */
#undef assert

#ifdef NDEBUG
#define assert(e) ((void)0)
#else
__attribute__((noreturn))
void __assert_fail(const char *expr, const char *file, int line, const char *func);
#define assert(e) ((e) ? (void)0 : __assert_fail(#e, __FILE__, __LINE__, __func__))
#endif
