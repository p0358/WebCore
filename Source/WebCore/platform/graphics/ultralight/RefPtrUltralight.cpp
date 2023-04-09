#include "config.h"
#include "RefPtrUltralight.h"

#if USE(ULTRALIGHT)

#include <Ultralight/private/Path.h>

namespace WTF {

#define DEFINE_REF_DEREF_ULTRALIGHT(X)                    \
    void DefaultRefDerefTraits<X>::refIfNotNull(X* ptr)   \
    {                                                     \
        if (LIKELY(ptr))                                  \
            ptr->AddRef();                                \
    }                                                     \
    void DefaultRefDerefTraits<X>::derefIfNotNull(X* ptr) \
    {                                                     \
        if (LIKELY(ptr))                                  \
            ptr->Release();                               \
    }

DEFINE_REF_DEREF_ULTRALIGHT(ultralight::Path)

}  // namespace WTF

#endif  // USE(ULTRALIGHT)
