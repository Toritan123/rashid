/*
 * Rashid - run Intel (x86_64) macOS applications on Apple Silicon
 * Copyright 2026 Toritan123
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
// rashid - stubs for imports that have no thunk yet
//
// Every imported symbol is bound to a distinct address in a small executable
// region. Jumping to one is not a crash but a diagnostic: rashid reports the
// symbol and the library it came from. That turns "this application does not
// run" into a worklist.
#ifndef RASHID_STUBS_H
#define RASHID_STUBS_H

#include <stdint.h>

#define RSD_STUB_STRIDE 16

// A variadic function rashid knows how to forward. macOS arm64 puts every
// variadic argument on the stack, while System V puts the first few in
// registers, so the two cannot be bridged without knowing how many arguments
// there are and what they are - which for these functions means reading the
// format string.
typedef struct {
    int  nfixed;   // arguments before the ellipsis
    int  fmt;      // which of them is the format string
    bool scan;     // scanf family: every conversion consumes a pointer
    bool nsformat; // the format is an NSString, not a C string
    bool objc;     // objc_msgSend: whether it is variadic depends on the
                   // selector, so that is decided at call time
} rsd_vaspec;

// Borrow a C string from an NSString the guest handed us. The object is a
// native one, so this is an ordinary message send.
const char *rsd_objc_cstring(uint64_t nsstring);

// If this selector names a variadic method, the number of fixed arguments
// including self and _cmd, with the format string last. Zero otherwise.
int rsd_objc_variadic_sel(uint64_t sel);

// An image's own class structures are handles, not real classes. Wherever the
// guest hands one across the boundary it has to become the class the runtime
// actually registered.
uint64_t rsd_objc_real_class(uint64_t maybe_guest_class);

const rsd_vaspec *rsd_variadic_spec(const char *symbol);

// Which argument of this function, if any, is a callback the guest supplies.
// Native code cannot call guest code directly, so such an argument has to be
// replaced with a trampoline before the call goes out.
int  rsd_callback_arg(const char *symbol);
bool rsd_takes_objc_receiver(const char *symbol);

typedef struct {
    uint64_t     base;      // guest address of stub 0
    int          n;
    const char **names;     // symbol name per stub
    const char **libs;      // library it was imported from
    void       **fns;       // native arm64 implementation, or NULL
    const signed char *cbarg; // argument index that is a guest function
                              // pointer, or -1
    const unsigned char *objcrecv; // takes an Objective-C receiver in arg 0
    const rsd_vaspec **va;  // non-NULL where the function is variadic
} rsd_stubs;

// The call gate, implemented in callgate.S: enter a native arm64 function
// with a chosen register and stack state, capturing both return registers.
void rsd_call_native(void *fn, const uint64_t x[8], const double d[8],
                     const void *stack, uint64_t stackbytes, uint64_t ret[4]);

// Which import does this address belong to, or -1.
static inline int rsd_stub_index(const rsd_stubs *s, uint64_t addr) {
    if (!s || !s->base || addr < s->base) return -1;
    uint64_t off = addr - s->base;
    if (off >= (uint64_t)s->n * RSD_STUB_STRIDE) return -1;
    return (int)(off / RSD_STUB_STRIDE);
}

#endif
