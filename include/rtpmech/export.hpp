// =============================================================================
//  rtpmech/export.hpp —— 共享库导出宏
// =============================================================================
//  构建共享库：cmake -DRTPMECH_LIBRARY_TYPE=SHARED
//    · 库本身定义 RTPMECH_BUILDING_LIBRARY → dllexport
//    · 使用方定义 RTPMECH_SHARED → dllimport（Windows）
//  Linux 配合 CXX_VISIBILITY_PRESET=hidden，只导出标了 RTPMECH_API 的符号。
//  STATIC（默认）展开为空，不引入任何负担。
// =============================================================================
#pragma once

#if defined(_WIN32) && defined(RTPMECH_SHARED)
#  if defined(RTPMECH_BUILDING_LIBRARY)
#    define RTPMECH_API __declspec(dllexport)
#  else
#    define RTPMECH_API __declspec(dllimport)
#  endif
#elif defined(__GNUC__) && defined(RTPMECH_SHARED)
#  define RTPMECH_API __attribute__((visibility("default")))
#else
#  define RTPMECH_API
#endif
