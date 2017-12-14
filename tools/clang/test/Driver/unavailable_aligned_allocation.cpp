// RUN: %clang -target x86_64-apple-macosx10.12 -c -### %s 2>&1 \
// RUN:   | FileCheck %s -check-prefix=UNAVAILABLE
//
// RUN: %clang -target arm64-apple-ios10 -c -### %s 2>&1 \
// RUN:   | FileCheck %s -check-prefix=UNAVAILABLE
//
// RUN: %clang -target arm64-apple-tvos10 -c -### %s 2>&1 \
// RUN:   | FileCheck %s -check-prefix=UNAVAILABLE
//
// RUN: %clang -target thumbv7-apple-watchos3 -c -### %s 2>&1 \
// RUN:   | FileCheck %s -check-prefix=UNAVAILABLE
//
// RUN: %clang -target x86_64-apple-macosx10.13 -mios-simulator-version-min=10 \
// RUN:  -c -### %s 2>&1 \
// RUN:   | FileCheck %s -check-prefix=UNAVAILABLE
//
// RUN: %clang -target x86_64-apple-macosx10.13 -mtvos-simulator-version-min=10 \
// RUN: -c -### %s 2>&1 \
// RUN:   | FileCheck %s -check-prefix=UNAVAILABLE
//
// RUN: %clang -target x86_64-apple-macosx10.13 -mwatchos-simulator-version-min=3 \
// RUN: -c -### %s 2>&1 \
// RUN:   | FileCheck %s -check-prefix=UNAVAILABLE
//
// UNAVAILABLE: "-faligned-alloc-unavailable"

// RUN: %clang -target x86_64-apple-macosx10.13 -c -### %s 2>&1 \
// RUN:   | FileCheck %s -check-prefix=AVAILABLE
//
// RUN: %clang -target arm64-apple-ios11 -c -### %s 2>&1 \
// RUN:   | FileCheck %s -check-prefix=AVAILABLE
//
// RUN: %clang -target arm64-apple-tvos11 -c -### %s 2>&1 \
// RUN:   | FileCheck %s -check-prefix=AVAILABLE
//
// RUN: %clang -target armv7k-apple-watchos4 -c -### %s 2>&1 \
// RUN:   | FileCheck %s -check-prefix=AVAILABLE
//
// RUN: %clang -target x86_64-unknown-linux-gnu -c -### %s 2>&1 \
// RUN:   | FileCheck %s -check-prefix=AVAILABLE
//
// RUN: %clang -target x86_64-apple-macosx10.12 -mios-simulator-version-min=11 \
// RUN:  -c -### %s 2>&1 \
// RUN:   | FileCheck %s -check-prefix=AVAILABLE
//
// RUN: %clang -target x86_64-apple-macosx10.12 -mtvos-simulator-version-min=11 \
// RUN: -c -### %s 2>&1 \
// RUN:   | FileCheck %s -check-prefix=AVAILABLE
//
// RUN: %clang -target x86_64-apple-macosx10.12 -mwatchos-simulator-version-min=4 \
// RUN: -c -### %s 2>&1 \
// RUN:   | FileCheck %s -check-prefix=AVAILABLE
//
// AVAILABLE-NOT: "-faligned-alloc-unavailable"
