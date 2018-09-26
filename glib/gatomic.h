/*
 * Copyright © 2011 Ryan Lortie
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 *
 * Author: Ryan Lortie <desrt@desrt.ca>
 */

#ifndef __G_ATOMIC_H__
#define __G_ATOMIC_H__

#if !defined (__GLIB_H_INSIDE__) && !defined (GLIB_COMPILATION)
#error "Only <glib.h> can be included directly."
#endif

#include <glib/gtypes.h>

G_BEGIN_DECLS

GLIB_AVAILABLE_IN_ALL
gint                    g_atomic_int_get                      (const volatile gint *atomic);
GLIB_AVAILABLE_IN_ALL
void                    g_atomic_int_set                      (volatile gint  *atomic,
                                                               gint            newval);
GLIB_AVAILABLE_IN_ALL
void                    g_atomic_int_inc                      (volatile gint  *atomic);
GLIB_AVAILABLE_IN_ALL
gboolean                g_atomic_int_dec_and_test             (volatile gint  *atomic);
GLIB_AVAILABLE_IN_ALL
gboolean                g_atomic_int_compare_and_exchange     (volatile gint  *atomic,
                                                               gint            oldval,
                                                               gint            newval);
GLIB_AVAILABLE_IN_ALL
gint                    g_atomic_int_add                      (volatile gint  *atomic,
                                                               gint            val);
GLIB_AVAILABLE_IN_2_30
guint                   g_atomic_int_and                      (volatile guint *atomic,
                                                               guint           val);
GLIB_AVAILABLE_IN_2_30
guint                   g_atomic_int_or                       (volatile guint *atomic,
                                                               guint           val);
GLIB_AVAILABLE_IN_ALL
guint                   g_atomic_int_xor                      (volatile guint *atomic,
                                                               guint           val);

GLIB_AVAILABLE_IN_ALL
gpointer                g_atomic_pointer_get                  (const volatile void *atomic);
GLIB_AVAILABLE_IN_ALL
void                    g_atomic_pointer_set                  (volatile void  *atomic,
                                                               gpointer        newval);
GLIB_AVAILABLE_IN_ALL
gboolean                g_atomic_pointer_compare_and_exchange (volatile void  *atomic,
                                                               gpointer        oldval,
                                                               gpointer        newval);
GLIB_AVAILABLE_IN_ALL
gssize                  g_atomic_pointer_add                  (volatile void  *atomic,
                                                               gssize          val);
GLIB_AVAILABLE_IN_2_30
gsize                   g_atomic_pointer_and                  (volatile void  *atomic,
                                                               gsize           val);
GLIB_AVAILABLE_IN_2_30
gsize                   g_atomic_pointer_or                   (volatile void  *atomic,
                                                               gsize           val);
GLIB_AVAILABLE_IN_ALL
gsize                   g_atomic_pointer_xor                  (volatile void  *atomic,
                                                               gsize           val);

GLIB_DEPRECATED_IN_2_30_FOR(g_atomic_int_add)
gint                    g_atomic_int_exchange_and_add         (volatile gint  *atomic,
                                                               gint            val);

G_END_DECLS

#if defined(G_ATOMIC_LOCK_FREE) && defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_4)

/* We prefer the new C11-style atomic extension of GCC if available */
#if defined(__ATOMIC_SEQ_CST) && !defined(__clang__)

/* This assumes sizeof(int) is 4: gatomic.c statically
 * asserts that (using G_STATIC_ASSERT at top-level in a header was
 * problematic, see #730932) */

#define g_atomic_int_get(atomic) \
  (G_GNUC_EXTENSION ({                                                       \
    G_STATIC_ASSERT (sizeof *(atomic) == sizeof (gint));                     \
    (void) (0 ? *(atomic) ^ *(atomic) : 1);                                  \
    (gint) __atomic_load_4 ((atomic), __ATOMIC_SEQ_CST);                     \
  }))
#define g_atomic_int_set(atomic, newval) \
  (G_GNUC_EXTENSION ({                                                       \
    G_STATIC_ASSERT (sizeof *(atomic) == sizeof (gint));                     \
    (void) (0 ? *(atomic) ^ (newval) : 1);                                   \
    __atomic_store_4 ((atomic), (newval), __ATOMIC_SEQ_CST);                 \
  }))

#if GLIB_SIZEOF_VOID_P == 8

#define g_atomic_pointer_get(atomic) \
  (G_GNUC_EXTENSION ({                                                       \
    G_STATIC_ASSERT (sizeof *(atomic) == sizeof (gpointer));                 \
    (gpointer) __atomic_load_8 ((atomic), __ATOMIC_SEQ_CST);                 \
  }))
#define g_atomic_pointer_set(atomic, newval) \
  (G_GNUC_EXTENSION ({                                                       \
    G_STATIC_ASSERT (sizeof *(atomic) == sizeof (gpointer));                 \
    (void) (0 ? (gpointer) *(atomic) : NULL);                                \
    __atomic_store_8 ((atomic), (gsize) (newval), __ATOMIC_SEQ_CST);         \
  }))

#else /* GLIB_SIZEOF_VOID_P == 8 */

/* This assumes that if sizeof(void *) is not 8, then it is 4:
 * gatomic.c statically asserts that (using G_STATIC_ASSERT
 * at top-level in a header was problematic, see #730932) */

#define g_atomic_pointer_get(atomic) \
  (G_GNUC_EXTENSION ({                                                       \
    G_STATIC_ASSERT (sizeof *(atomic) == sizeof (gpointer));                 \
    (gpointer) __atomic_load_4 ((atomic), __ATOMIC_SEQ_CST);                 \
  }))
#define g_atomic_pointer_set(atomic, newval) \
  (G_GNUC_EXTENSION ({                                                       \
    G_STATIC_ASSERT (sizeof *(atomic) == sizeof (gpointer));                 \
    (void) (0 ? (gpointer) *(atomic) : NULL);                                \
    __atomic_store_4 ((atomic), (gsize) (newval), __ATOMIC_SEQ_CST);         \
  }))

#endif /* GLIB_SIZEOF_VOID_P == 8 */

#else /* defined(__ATOMIC_SEQ_CST) */

#define g_atomic_int_get(atomic) \
  (G_GNUC_EXTENSION ({                                                       \
    G_STATIC_ASSERT (sizeof *(atomic) == sizeof (gint));                     \
    (void) (0 ? *(atomic) ^ *(atomic) : 1);                                  \
    __sync_synchronize ();                                                   \
    (gint) *(atomic);                                                        \
  }))
#define g_atomic_int_set(atomic, newval) \
  (G_GNUC_EXTENSION ({                                                       \
    G_STATIC_ASSERT (sizeof *(atomic) == sizeof (gint));                     \
    (void) (0 ? *(atomic) ^ (newval) : 1);                                   \
    *(atomic) = (newval);                                                    \
    __sync_synchronize ();                                                   \
  }))
#define g_atomic_pointer_get(atomic) \
  (G_GNUC_EXTENSION ({                                                       \
    G_STATIC_ASSERT (sizeof *(atomic) == sizeof (gpointer));                 \
    __sync_synchronize ();                                                   \
    (gpointer) *(atomic);                                                    \
  }))
#define g_atomic_pointer_set(atomic, newval) \
  (G_GNUC_EXTENSION ({                                                       \
    G_STATIC_ASSERT (sizeof *(atomic) == sizeof (gpointer));                 \
    (void) (0 ? (gpointer) *(atomic) : NULL);                                \
    *(atomic) = (__typeof__ (*(atomic))) (gsize) (newval);                   \
    __sync_synchronize ();                                                   \
  }))

#endif /* !defined(__ATOMIC_SEQ_CST) */

#define g_atomic_int_inc(atomic) \
  (G_GNUC_EXTENSION ({                                                       \
    G_STATIC_ASSERT (sizeof *(atomic) == sizeof (gint));                     \
    (void) (0 ? *(atomic) ^ *(atomic) : 1);                                  \
    (void) __sync_fetch_and_add ((atomic), 1);                               \
  }))
#define g_atomic_int_dec_and_test(atomic) \
  (G_GNUC_EXTENSION ({                                                       \
    G_STATIC_ASSERT (sizeof *(atomic) == sizeof (gint));                     \
    (void) (0 ? *(atomic) ^ *(atomic) : 1);                                  \
    __sync_fetch_and_sub ((atomic), 1) == 1;                                 \
  }))
#define g_atomic_int_compare_and_exchange(atomic, oldval, newval) \
  (G_GNUC_EXTENSION ({                                                       \
    G_STATIC_ASSERT (sizeof *(atomic) == sizeof (gint));                     \
    (void) (0 ? *(atomic) ^ (newval) ^ (oldval) : 1);                        \
    (gboolean) __sync_bool_compare_and_swap ((atomic), (oldval), (newval));  \
  }))
#define g_atomic_int_add(atomic, val) \
  (G_GNUC_EXTENSION ({                                                       \
    G_STATIC_ASSERT (sizeof *(atomic) == sizeof (gint));                     \
    (void) (0 ? *(atomic) ^ (val) : 1);                                      \
    (gint) __sync_fetch_and_add ((atomic), (val));                           \
  }))
#define g_atomic_int_and(atomic, val) \
  (G_GNUC_EXTENSION ({                                                       \
    G_STATIC_ASSERT (sizeof *(atomic) == sizeof (gint));                     \
    (void) (0 ? *(atomic) ^ (val) : 1);                                      \
    (guint) __sync_fetch_and_and ((atomic), (val));                          \
  }))
#define g_atomic_int_or(atomic, val) \
  (G_GNUC_EXTENSION ({                                                       \
    G_STATIC_ASSERT (sizeof *(atomic) == sizeof (gint));                     \
    (void) (0 ? *(atomic) ^ (val) : 1);                                      \
    (guint) __sync_fetch_and_or ((atomic), (val));                           \
  }))
#define g_atomic_int_xor(atomic, val) \
  (G_GNUC_EXTENSION ({                                                       \
    G_STATIC_ASSERT (sizeof *(atomic) == sizeof (gint));                     \
    (void) (0 ? *(atomic) ^ (val) : 1);                                      \
    (guint) __sync_fetch_and_xor ((atomic), (val));                          \
  }))

#define g_atomic_pointer_compare_and_exchange(atomic, oldval, newval) \
  (G_GNUC_EXTENSION ({                                                       \
    G_STATIC_ASSERT (sizeof *(atomic) == sizeof (gpointer));                 \
    (void) (0 ? (gpointer) *(atomic) : NULL);                                \
    (gboolean) __sync_bool_compare_and_swap ((atomic), (oldval), (newval));  \
  }))
#define g_atomic_pointer_add(atomic, val) \
  (G_GNUC_EXTENSION ({                                                       \
    G_STATIC_ASSERT (sizeof *(atomic) == sizeof (gpointer));                 \
    (void) (0 ? (gpointer) *(atomic) : NULL);                                \
    (void) (0 ? (val) ^ (val) : 1);                                          \
    (gssize) __sync_fetch_and_add ((atomic), (val));                         \
  }))
#define g_atomic_pointer_and(atomic, val) \
  (G_GNUC_EXTENSION ({                                                       \
    G_STATIC_ASSERT (sizeof *(atomic) == sizeof (gpointer));                 \
    (void) (0 ? (gpointer) *(atomic) : NULL);                                \
    (void) (0 ? (val) ^ (val) : 1);                                          \
    (gsize) __sync_fetch_and_and ((atomic), (val));                          \
  }))
#define g_atomic_pointer_or(atomic, val) \
  (G_GNUC_EXTENSION ({                                                       \
    G_STATIC_ASSERT (sizeof *(atomic) == sizeof (gpointer));                 \
    (void) (0 ? (gpointer) *(atomic) : NULL);                                \
    (void) (0 ? (val) ^ (val) : 1);                                          \
    (gsize) __sync_fetch_and_or ((atomic), (val));                           \
  }))
#define g_atomic_pointer_xor(atomic, val) \
  (G_GNUC_EXTENSION ({                                                       \
    G_STATIC_ASSERT (sizeof *(atomic) == sizeof (gpointer));                 \
    (void) (0 ? (gpointer) *(atomic) : NULL);                                \
    (void) (0 ? (val) ^ (val) : 1);                                          \
    (gsize) __sync_fetch_and_xor ((atomic), (val));                          \
  }))

#else /* defined(G_ATOMIC_LOCK_FREE) && defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_4) */

#define g_atomic_int_get(atomic) \
  (g_atomic_int_get ((gint *) (atomic)))
#define g_atomic_int_set(atomic, newval) \
  (g_atomic_int_set ((gint *) (atomic), (gint) (newval)))
#define g_atomic_int_compare_and_exchange(atomic, oldval, newval) \
  (g_atomic_int_compare_and_exchange ((gint *) (atomic), (oldval), (newval)))
#define g_atomic_int_add(atomic, val) \
  (g_atomic_int_add ((gint *) (atomic), (val)))
#define g_atomic_int_and(atomic, val) \
  (g_atomic_int_and ((guint *) (atomic), (val)))
#define g_atomic_int_or(atomic, val) \
  (g_atomic_int_or ((guint *) (atomic), (val)))
#define g_atomic_int_xor(atomic, val) \
  (g_atomic_int_xor ((guint *) (atomic), (val)))
#define g_atomic_int_inc(atomic) \
  (g_atomic_int_inc ((gint *) (atomic)))
#define g_atomic_int_dec_and_test(atomic) \
  (g_atomic_int_dec_and_test ((gint *) (atomic)))

#define g_atomic_pointer_get(atomic) \
  (g_atomic_pointer_get (atomic))
#define g_atomic_pointer_set(atomic, newval) \
  (g_atomic_pointer_set ((atomic), (gpointer) (newval)))
#define g_atomic_pointer_compare_and_exchange(atomic, oldval, newval) \
  (g_atomic_pointer_compare_and_exchange ((atomic), (gpointer) (oldval), (gpointer) (newval)))
#define g_atomic_pointer_add(atomic, val) \
  (g_atomic_pointer_add ((atomic), (gssize) (val)))
#define g_atomic_pointer_and(atomic, val) \
  (g_atomic_pointer_and ((atomic), (gsize) (val)))
#define g_atomic_pointer_or(atomic, val) \
  (g_atomic_pointer_or ((atomic), (gsize) (val)))
#define g_atomic_pointer_xor(atomic, val) \
  (g_atomic_pointer_xor ((atomic), (gsize) (val)))

#endif /* defined(__GNUC__) && defined(G_ATOMIC_OP_USE_GCC_BUILTINS) */

#if defined(G_PLATFORM_WIN32)

#pragma intrinsic(__faststorefence)
#pragma intrinsic(_InterlockedCompareExchange)
#pragma intrinsic(_InterlockedIncrement)
#pragma intrinsic(_InterlockedDecrement)
#pragma intrinsic(_InterlockedCompareExchange)
#pragma intrinsic(_InterlockedExchangeAdd)
#pragma intrinsic(_InterlockedCompareExchangePointer)
#pragma intrinsic(_InterlockedExchangeAdd64)
#pragma intrinsic(_InterlockedAnd64)
#pragma intrinsic(_InterlockedOr64)
#pragma intrinsic(_InterlockedXor64)

#if !defined (_MSC_VER) || _MSC_VER <= 1200
/* Inlined versions for older compiler */
static inline LONG
_gInterlockedAnd (volatile guint *atomic,
	guint           val)
{
	LONG i, j;

	j = *atomic;
	do {
		i = j;
		j = _InterlockedCompareExchange (atomic, i & val, i);
	} while (i != j);

	return j;
}
#define InterlockedAnd(a,b) _gInterlockedAnd(a,b)
static inline LONG
_gInterlockedOr (volatile guint *atomic,
	guint           val)
{
	LONG i, j;

	j = *atomic;
	do {
		i = j;
		j = _InterlockedCompareExchange (atomic, i | val, i);
	} while (i != j);

	return j;
}
#define InterlockedOr(a,b) _gInterlockedOr(a,b)
static inline LONG
_gInterlockedXor (volatile guint *atomic,
	guint           val)
{
	LONG i, j;

	j = *atomic;
	do {
		i = j;
		j = _InterlockedCompareExchange (atomic, i ^ val, i);
	} while (i != j);

	return j;
}
#define _InterlockedXor(a,b) _gInterlockedXor(a,b)

#else

#pragma intrinsic(_InterlockedAnd)
#pragma intrinsic(_InterlockedOr)
#pragma intrinsic(_InterlockedXor)

#endif /* !defined (_MSC_VER) || _MSC_VER <= 1200 */

/*
 * http://msdn.microsoft.com/en-us/library/ms684122(v=vs.85).aspx
 */
static inline gint
(g_atomic_int_get) (const volatile gint *atomic)
{
	__faststorefence ();
	return *atomic;
}

static inline void
(g_atomic_int_set)(volatile gint *atomic,
	gint           newval)
{
	*atomic = newval;
	__faststorefence ();
}

static inline void
(g_atomic_int_inc)(volatile gint *atomic)
{
	_InterlockedIncrement (atomic);
}

static inline gboolean
(g_atomic_int_dec_and_test) (volatile gint *atomic)
{
	return _InterlockedDecrement (atomic) == 0;
}

static inline gboolean
(g_atomic_int_compare_and_exchange) (volatile gint *atomic,
	gint           oldval,
	gint           newval)
{
	return _InterlockedCompareExchange (atomic, newval, oldval) == oldval;
}

static inline gint
(g_atomic_int_add) (volatile gint *atomic,
	gint           val)
{
	return _InterlockedExchangeAdd (atomic, val);
}

static inline guint
(g_atomic_int_and) (volatile guint *atomic,
	guint           val)
{
	return _InterlockedAnd (atomic, val);
}

static inline guint
(g_atomic_int_or) (volatile guint *atomic,
	guint           val)
{
	return _InterlockedOr (atomic, val);
}

static inline guint
(g_atomic_int_xor) (volatile guint *atomic,
	guint           val)
{
	return _InterlockedXor (atomic, val);
}

static inline gpointer
(g_atomic_pointer_get) (const volatile void *atomic)
{
	const volatile gpointer *ptr = (const volatile gpointer *)atomic;

	__faststorefence ();
	return *ptr;
}

static inline void
(g_atomic_pointer_set)(volatile void *atomic,
	gpointer       newval)
{
	volatile gpointer *ptr = (volatile gpointer *)atomic;

	*ptr = newval;
	__faststorefence ();
}

static inline gboolean
(g_atomic_pointer_compare_and_exchange) (volatile void *atomic,
	gpointer       oldval,
	gpointer       newval)
{
	return _InterlockedCompareExchangePointer (atomic, newval, oldval) == oldval;
}

static inline gssize
(g_atomic_pointer_add) (volatile void *atomic,
	gssize         val)
{
#if GLIB_SIZEOF_VOID_P == 8
	return _InterlockedExchangeAdd64 (atomic, val);
#else
	return _InterlockedExchangeAdd (atomic, val);
#endif
}

static inline gsize
(g_atomic_pointer_and) (volatile void *atomic,
	gsize          val)
{
#if GLIB_SIZEOF_VOID_P == 8
	return _InterlockedAnd64 (atomic, val);
#else
	return _InterlockedAnd (atomic, val);
#endif
}

static inline gsize
(g_atomic_pointer_or) (volatile void *atomic,
	gsize          val)
{
#if GLIB_SIZEOF_VOID_P == 8
	return _InterlockedOr64 (atomic, val);
#else
	return _InterlockedOr (atomic, val);
#endif
}

static inline gsize
(g_atomic_pointer_xor) (volatile void *atomic,
	gsize          val)
{
#if GLIB_SIZEOF_VOID_P == 8
	return _InterlockedXor64 (atomic, val);
#else
	return _InterlockedXor (atomic, val);
#endif
}

#endif /* defined(G_PLATFORM_WIN32) */

#endif /* __G_ATOMIC_H__ */
