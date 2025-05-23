//===--- DiagnosticLex.h - Diagnostics for liblex ---------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_BASIC_DIAGNOSTICLEX_H
#define LLVM_CLANG_BASIC_DIAGNOSTICLEX_H

#include "clang/Basic/Diagnostic.h"

namespace clang {
namespace diag {
enum {
#define DIAG(ENUM, FLAGS, DEFAULT_MAPPING, DESC, GROUP, SFINAE, NOWERROR,      \
             SHOWINSYSHEADER, SHOWINSYSMACRO, DEFERRABLE, CATEGORY)            \
  ENUM,
#define LEXSTART
#include "clang/Basic/DiagnosticLexKinds.inc"
#undef DIAG
  NUM_BUILTIN_LEX_DIAGNOSTICS
};
#define DIAG_ENUM(ENUM_NAME)                                                   \
  namespace ENUM_NAME {                                                        \
  enum {
#define DIAG_ENUM_ITEM(IDX, NAME) NAME = IDX,
#define DIAG_ENUM_END()                                                        \
  }                                                                            \
  ;                                                                            \
  }
#include "clang/Basic/DiagnosticLexEnums.inc"
#undef DIAG_ENUM_END
#undef DIAG_ENUM_ITEM
#undef DIAG_ENUM
} // end namespace diag

namespace diag_compat {
#define DIAG_COMPAT_IDS_BEGIN() enum {
#define DIAG_COMPAT_IDS_END()                                                  \
  }                                                                            \
  ;
#define DIAG_COMPAT_ID(IDX, NAME, ...) NAME = IDX,
#include "clang/Basic/DiagnosticLexCompatIDs.inc"
#undef DIAG_COMPAT_ID
#undef DIAG_COMPAT_IDS_BEGIN
#undef DIAG_COMPAT_IDS_END
} // end namespace diag_compat
} // end namespace clang

#endif // LLVM_CLANG_BASIC_DIAGNOSTICLEX_H
