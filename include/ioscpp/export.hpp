#pragma once

#if defined(_WIN32) && defined(IOSCPP_SHARED)
#    if defined(IOSCPP_EXPORTS)
#        define IOSCPP_API __declspec(dllexport)
#    else
#        define IOSCPP_API __declspec(dllimport)
#    endif
#elif defined(__GNUC__) || defined(__clang__)
#    define IOSCPP_API __attribute__((visibility("default")))
#else
#    define IOSCPP_API
#endif
