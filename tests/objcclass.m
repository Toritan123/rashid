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
// Classes the application defines itself.
//
// This is where the two directions meet. The class is built through the
// public runtime API with every implementation replaced by a trampoline, so
// the native runtime dispatches a message and ends up in the interpreter,
// which then sends messages back out into Foundation.
//
// The image references its own classes by address - clang emits a direct
// leaq to _OBJC_CLASS_$_Foo - so those structures are treated as handles and
// swapped for the real class wherever the guest passes one across the
// boundary. Copying a finished class object over the image's own does not
// work: libobjc is arm64e and signs pointers with address diversity.
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <stdio.h>

@interface Counter : NSObject {
    int _hits;
    NSString *_label;
}
- (instancetype)initWithLabel:(NSString *)l;
- (void)bump;
- (int)hits;
- (NSString *)describe;
+ (NSString *)version;
@end

@implementation Counter
- (instancetype)initWithLabel:(NSString *)l {
    self = [super init];               // a message to the superclass
    if (self) { _hits = 0; _label = l; }
    return self;
}
- (void)bump { _hits++; }              // an ivar the runtime laid out
- (int)hits { return _hits; }
- (NSString *)describe {               // guest code calling back into Foundation
    return [NSString stringWithFormat:@"%@=%d", _label, _hits];
}
+ (NSString *)version { return @"1.0"; }
@end

int main(void) { @autoreleasepool {
    Counter *c = [[Counter alloc] initWithLabel:@"clicks"];
    for (int i = 0; i < 5; i++) [c bump];
    printf("hits=%d\n", [c hits]);
    printf("describe=%s\n", [[c describe] UTF8String]);
    printf("version=%s\n", [[Counter version] UTF8String]);
    printf("isa=%s\n", class_getName([c class]));
    printf("responds=%d\n", (int)[c respondsToSelector:@selector(bump)]);

    // The class is real enough for the runtime to find it by name.
    Class byName = NSClassFromString(@"Counter");
    printf("byname=%s\n", byName ? class_getName(byName) : "(null)");
    return 0;
} }
