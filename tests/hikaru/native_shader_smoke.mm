// Portions Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-3.0-or-later
#import <Metal/Metal.h>

#include "hw/hikaru/graphics/shaders.h"
#include <iostream>

int main() {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) {
            std::cout << "SKIP: no Metal device is available\n";
            return 77;
        }
        NSError *error = nil;
        NSString *source = [NSString stringWithUTF8String:hikaru::graphics::metalShaderSource().c_str()];
        id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
        if (!library) {
            std::cerr << "Metal shader compilation failed: "
                      << (error.localizedDescription.UTF8String ?: "unknown error") << "\n";
            return 1;
        }
        for (NSString *name in @[@"meshVertex", @"bitmapVertex", @"meshFragment", @"bitmapFragment"]) {
            if (![library newFunctionWithName:name]) {
                std::cerr << "Generated Metal library is missing function " << name.UTF8String << "\n";
                return 1;
            }
        }
        std::cout << "Generated Hikaru Metal shader compiled on " << device.name.UTF8String << "\n";
        return 0;
    }
}
