// Portability shims for the FidelityFX SDK sources built by cmake/fsr.
//
// The SDK uses the Microsoft secure CRT names in source that is also built on
// non-MSVC platforms.  Keep these definitions target-local by force-including
// this file only for the wrapper's SDK target.
#ifndef FFX_WRAPPER_PORTABILITY_H
#define FFX_WRAPPER_PORTABILITY_H

#ifndef _MSC_VER

#include <cerrno>
#include <cstddef>
#include <cstdarg>
#include <cmath>
#include <codecvt>
#include <cstdio>
#include <cwchar>
#include <cstring>
#include <locale>

#ifndef _countof
#define _countof(array) (sizeof(array) / sizeof((array)[0]))
#endif

inline int ffx_wrapper_vsnprintf_s(
    char* destination,
    std::size_t destination_size,
    const char* format,
    std::va_list arguments)
{
    if (destination == nullptr || destination_size == 0 || format == nullptr) {
        if (destination != nullptr && destination_size != 0) {
            destination[0] = '\0';
        }
        return EINVAL;
    }

    const int result = std::vsnprintf(destination, destination_size, format, arguments);
    if (result < 0 || static_cast<std::size_t>(result) >= destination_size) {
        destination[0] = '\0';
        return ERANGE;
    }
    return result;
}

inline int sprintf_s(char* destination, std::size_t destination_size, const char* format, ...)
{
    std::va_list arguments;
    va_start(arguments, format);
    const int result = ffx_wrapper_vsnprintf_s(destination, destination_size, format, arguments);
    va_end(arguments);
    return result;
}

inline int ffx_wrapper_vswprintf_s(
    wchar_t* destination,
    std::size_t destination_size,
    const wchar_t* format,
    std::va_list arguments)
{
    if (destination == nullptr || destination_size == 0 || format == nullptr) {
        if (destination != nullptr && destination_size != 0) {
            destination[0] = L'\0';
        }
        return EINVAL;
    }

    const int result = std::vswprintf(destination, destination_size, format, arguments);
    if (result < 0 || static_cast<std::size_t>(result) >= destination_size) {
        destination[0] = L'\0';
        return ERANGE;
    }
    return result;
}

inline int swprintf_s(
    wchar_t* destination,
    std::size_t destination_size,
    const wchar_t* format,
    ...)
{
    std::va_list arguments;
    va_start(arguments, format);
    const int result = ffx_wrapper_vswprintf_s(destination, destination_size, format, arguments);
    va_end(arguments);
    return result;
}

inline int strcpy_s(char* destination, std::size_t destination_size, const char* source)
{
    if (destination == nullptr || destination_size == 0 || source == nullptr) {
        if (destination != nullptr && destination_size != 0) {
            destination[0] = '\0';
        }
        return EINVAL;
    }

    const std::size_t source_length = std::strlen(source);
    if (source_length >= destination_size) {
        destination[0] = '\0';
        return ERANGE;
    }
    std::memcpy(destination, source, source_length + 1);
    return 0;
}

template <std::size_t DestinationSize>
inline int strcpy_s(char (&destination)[DestinationSize], const char* source)
{
    return strcpy_s(destination, DestinationSize, source);
}

inline int wcscpy_s(wchar_t* destination, std::size_t destination_size, const wchar_t* source)
{
    if (destination == nullptr || destination_size == 0 || source == nullptr) {
        if (destination != nullptr && destination_size != 0) {
            destination[0] = L'\0';
        }
        return EINVAL;
    }

    const std::size_t source_length = std::wcslen(source);
    if (source_length >= destination_size) {
        destination[0] = L'\0';
        return ERANGE;
    }
    std::wmemcpy(destination, source, source_length + 1);
    return 0;
}

template <std::size_t DestinationSize>
inline int wcscpy_s(wchar_t (&destination)[DestinationSize], const wchar_t* source)
{
    return wcscpy_s(destination, DestinationSize, source);
}

#endif // !_MSC_VER

#endif // FFX_WRAPPER_PORTABILITY_H
