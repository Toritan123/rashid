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
// Objective-C. This is the milestone the whole design was aimed at: an
// application's own code is translated, and every message it sends runs in
// the native arm64 Foundation already on the machine.
//
// objc_msgSend needs no special handling. It looks variadic but is not: the
// compiler knows each call's real signature and emits a normal call, so the
// System V and AAPCS64 register assignments line up and the generic thunk
// carries it.
//
// What did need work is here too - a 16-byte struct returned in a register
// pair, a double argument and return, and NSLog, which is variadic with an
// NSString format rather than a C string.
//
// Assertions print to stdout so the harness can compare them; NSLog writes to
// stderr, where the timestamp and process name differ by construction.
#import <Foundation/Foundation.h>
#include <stdio.h>

int main(void) { @autoreleasepool {
    NSString *s = @"hello objective-c world";
    printf("len=%lu\n", (unsigned long)[s length]);

    NSRange r = [s rangeOfString:@"objective"];      // 16-byte struct return
    printf("range=%lu,%lu\n", (unsigned long)r.location, (unsigned long)r.length);

    NSNumber *n = [NSNumber numberWithDouble:2.5];   // double in, double out
    printf("double=%.2f\n", [n doubleValue]);

    printf("prefix=%d\n", (int)[s hasPrefix:@"hello"]);

    NSArray *a = @[@"x", @"y", @"z"];
    printf("count=%lu first=%s\n", (unsigned long)[a count],
           [[a objectAtIndex:0] UTF8String]);

    NSDictionary *d = @{@"k": @"v"};
    printf("dict=%s\n", [[d objectForKey:@"k"] UTF8String]);

    NSString *built = [NSString stringWithFormat:@"%@/%d", @"built", 7];
    printf("format=%s\n", [built UTF8String]);

    NSLog(@"NSLog reached with %@ (%lu)", s, (unsigned long)[s length]);
    return 0;
} }
