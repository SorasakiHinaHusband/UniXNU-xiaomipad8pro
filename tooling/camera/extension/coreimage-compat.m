// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, Leo Zhang. All rights reserved.
// Select Core Image's legacy CIKL metadata path until native Metal has function reflection.
#import <Foundation/Foundation.h>
#include <unistd.h>

int main(int argc, const char **argv)
{
  @autoreleasepool {
    if (argc < 2 || getuid() != 501) return 2;
    NSString *mode = @(argv[1]);
    NSString *domain = @"com.apple.coreimage";
    NSString *directory = [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support/PianoCamera"];
#ifdef PIANO_CMIO_POLICY_HOST_TEST
    if (argc != 4) return 2;
    domain = @(argv[2]); directory = @(argv[3]);
#else
    if (argc != 2) return 2;
#endif
    if (![@[@"--apply", @"--check", @"--remove"] containsObject:mode]) return 2;
    NSString *path = [directory stringByAppendingPathComponent:@"coreimage-compat.plist"];
    NSString *key = @"CI_USE_MTL_DAG_FOR_CIKL_SRC";
    CFStringRef app = (__bridge CFStringRef)domain, property = (__bridge CFStringRef)key;
    id old = CFBridgingRelease(CFPreferencesCopyValue(property, app, kCFPreferencesCurrentUser, kCFPreferencesAnyHost));
    if ([mode isEqualToString:@"--check"]) {
      BOOL ok = old && CFGetTypeID((__bridge CFTypeRef)old) == CFBooleanGetTypeID() && ![old boolValue];
      puts(ok ? "CMIO_CIKL_LEGACY_READY policy=cmio-apps2" : "CMIO_CIKL_POLICY_NOT_READY");
      return ok ? 0 : 1;
    }
    NSDictionary *receipt = [NSDictionary dictionaryWithContentsOfURL:[NSURL fileURLWithPath:path] error:NULL];
    if ([mode isEqualToString:@"--remove"]) {
      if (!receipt) { puts("CMIO_CIKL_POLICY_UNCHANGED"); return 0; }
      if (![receipt[@"domain"] isEqualToString:domain] || ![receipt[@"key"] isEqualToString:key] ||
          ![receipt[@"existed"] isKindOfClass:[NSNumber class]]) return 1;
      id restored = [receipt[@"existed"] boolValue] ? receipt[@"value"] : nil;
      if ([receipt[@"existed"] boolValue] && !restored) return 1;
      CFPreferencesSetValue(property, (__bridge CFPropertyListRef)restored, app, kCFPreferencesCurrentUser, kCFPreferencesAnyHost);
      if (!CFPreferencesSynchronize(app, kCFPreferencesCurrentUser, kCFPreferencesAnyHost)) return 1;
      id verified = CFBridgingRelease(CFPreferencesCopyValue(property, app, kCFPreferencesCurrentUser, kCFPreferencesAnyHost));
      if (restored ? ![restored isEqual:verified] : verified != nil) return 1;
      if (![[NSFileManager defaultManager] removeItemAtPath:path error:NULL]) return 1;
      puts("CMIO_CIKL_POLICY_RESTORED"); return 0;
    }
    if (!receipt) {
      if (![[NSFileManager defaultManager] createDirectoryAtPath:directory withIntermediateDirectories:YES
               attributes:@{NSFilePosixPermissions:@0700} error:NULL]) return 1;
      NSMutableDictionary *saved = [@{@"domain":domain, @"key":key, @"existed":@(old != nil)} mutableCopy];
      if (old) saved[@"value"] = old;
      NSData *data = [NSPropertyListSerialization dataWithPropertyList:saved format:NSPropertyListBinaryFormat_v1_0 options:0 error:NULL];
      if (!data || ![data writeToFile:path options:NSDataWritingAtomic error:NULL]) return 1;
      if (![[NSFileManager defaultManager] setAttributes:@{NSFilePosixPermissions:@0600} ofItemAtPath:path error:NULL]) return 1;
    } else if (![receipt[@"domain"] isEqualToString:domain] || ![receipt[@"key"] isEqualToString:key]) return 1;
    CFPreferencesSetValue(property, kCFBooleanFalse, app, kCFPreferencesCurrentUser, kCFPreferencesAnyHost);
    if (!CFPreferencesSynchronize(app, kCFPreferencesCurrentUser, kCFPreferencesAnyHost)) return 1;
    id verified = CFBridgingRelease(CFPreferencesCopyValue(property, app, kCFPreferencesCurrentUser, kCFPreferencesAnyHost));
    if (!verified || CFGetTypeID((__bridge CFTypeRef)verified) != CFBooleanGetTypeID() || [verified boolValue]) return 1;
    puts("CMIO_CIKL_LEGACY_READY policy=cmio-apps2");
  }
  return 0;
}
